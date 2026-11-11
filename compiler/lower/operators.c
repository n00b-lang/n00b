#include "lower_internal.h"

static ir_lower_result_t
n00bb_emit_comparison_test(ir_lower_context_t *context, ir_value_t *value,
                           parse_sym_entry_t *member, ir_span_t span);
static ir_lower_result_t
n00bb_lower_comparison(ir_lower_context_t *context,
                       ir_value_t *const *operands, th_type *result_type,
                       ir_span_t span, core_string_t spelling);
static bool
n00bb_literal_fits_machine(ir_lit_t literal, ir_machine_t machine);
static ir_value_t *
n00bb_constant_as(ir_lower_context_t *context, ir_value_t *value,
                  th_type *target, ir_span_t span);
static bool
n00bb_nil_literal(ir_value_t *value);
static ir_value_t *
n00bb_emit_value_instruction(ir_lower_context_t *context, ir_op_t op,
                             th_type *type, ir_value_t *left,
                             ir_value_t *right, ir_span_t span);
static ir_lower_result_t
n00bb_lower_enum_from_value(ir_lower_context_t *context,
                            n00bb_lower_context_t *lower,
                            parse_sym_entry_t *enum_symbol,
                            ir_value_t *operand, th_type *target,
                            ir_span_t span);
static ir_lower_result_t
report_undefined_cast(ir_lower_context_t *context, th_ctx *types,
                      th_type *from, th_type *target, ir_span_t span);
static th_type *
structural_shape(n00bb_tu_state_t *state, th_type *type, bool *named,
                 th_type **object_shape);
static bool
structural_field_fits(ir_lower_context_t *context, th_type *from,
                      th_type *to);
static ir_lower_result_t
report_tuple_cast_field(ir_lower_context_t *context, th_type *from,
                        th_type *target, core_string_t field, ir_span_t span);
static bool
n00bb_lower_structural_cast(ir_lower_context_t *context, ir_value_t *value,
                            th_type *target, bool unsafe_cast, ir_span_t span,
                            ir_lower_result_t *result);

ir_value_t *
n00bb_emit_binary_ir_with(ir_lower_context_t *context, ir_op_t op,
                          bool wrapping, th_type *result_type,
                          ir_value_t *left, ir_value_t *right, ir_span_t span)
{
    ir_instr_t *instruction = ir_lower_emit_instruction(context, op, span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), result_type)
        || !ir_instr_operand_value(instruction, left)
        || !ir_instr_operand_value(instruction, right)) return nullptr;
    if (n00bb_open_type(context, result_type)
        || n00bb_open_type(context, left->type))
        n00bb_rewrite_later(context, instruction);
    ir_class_t result_class = ir_machine_of(context->module, result_type).class;
    ir_class_t operand_class = ir_machine_of(context->module, left->type).class;
    bool integer_operation = result_class == IR_CLASS_SINT
        || result_class == IR_CLASS_UINT || operand_class == IR_CLASS_SINT
        || operand_class == IR_CLASS_UINT;
    if (wrapping)
        return integer_operation && (!ir_op_accepts_mod(op, CORE_STRING_STATIC("wrap"))
                || ir_instr_add_mod(instruction, CORE_STRING_STATIC("wrap")))
            ? instruction->dest : nullptr;
    if (integer_operation && !n00bb_trap_integer_arithmetic(instruction,
            result_class == IR_CLASS_SINT || operand_class == IR_CLASS_SINT
                ? IR_CLASS_SINT : IR_CLASS_UINT)) return nullptr;
    if (operand_class == IR_CLASS_FLOAT
        && (op == IR_OP_EQ || op == IR_OP_NE || op == IR_OP_LT
            || op == IR_OP_LE || op == IR_OP_GT || op == IR_OP_GE)
        && !ir_instr_add_mod(instruction, CORE_STRING_STATIC("ordered"))) return nullptr;
    return instruction->dest;
}

ir_value_t *
n00bb_emit_binary_ir(ir_lower_context_t *context, ir_op_t op,
                     th_type *result_type, ir_value_t *left,
                     ir_value_t *right, ir_span_t span)
{
    return n00bb_emit_binary_ir_with(context, op, false, result_type, left,
                                     right, span);
}

bool
n00bb_machine_promotes(ir_machine_t source, ir_machine_t destination)
{
    return source.class == destination.class
        ? destination.width >= source.width
        : source.class == IR_CLASS_UINT && destination.class == IR_CLASS_SINT
            && destination.width > source.width;
}

ir_value_t *
n00bb_widen_integer_operand(ir_lower_context_t *context, ir_value_t *value,
                            th_type *result_type, ir_span_t span)
{
    if (ir_type_eq(context->module, value->type, result_type)) return value;
    ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST, span);
    if (n00bb_open_type(context, value->type)
        || n00bb_open_type(context, result_type))
        n00bb_rewrite_later(context, cast);
    bool checked = !n00bb_machine_promotes(
        ir_machine_of(context->module, value->type),
        ir_machine_of(context->module, result_type));
    return cast && ir_lower_set_temp_dest(context, cast,
                       CORE_STRING_STATIC("__n00bb_"), result_type)
        && ir_instr_operand_value(cast, value)
        && (!checked || ir_instr_add_mod(cast, CORE_STRING_STATIC("trap")))
        ? cast->dest : nullptr;
}

th_type *
n00bb_ir_builtin_type(ir_module_t *module, core_string_t name)
{
    th_ctx *types = module ? ir_module_types(module) : nullptr;
    th_type *type = types && name.data ? th_name(types, name) : nullptr;
    return type ? ir_type_intern(module, type) : nullptr;
}

ir_value_t *
n00bb_emit_enum_member(ir_lower_context_t *context, parse_sym_entry_t *member,
                       ir_span_t span)
{
    n00bb_enum_value_t *ordinal = member && member->has_comptime_value
        ? member->comptime_value : nullptr;
    ir_instr_t *constant = ordinal && member->type
        ? ir_lower_emit_instruction(context, IR_OP_ENUM_CONST, span)
        : nullptr;
    return constant && ir_lower_set_temp_dest(context, constant,
            CORE_STRING_STATIC("__n00bb_"), member->type)
        && ir_instr_operand_name(constant, member->name)
        && ir_instr_operand_int_lit(constant,
            (ir_lit_t){.mag = ordinal->magnitude, .neg = ordinal->negative})
        ? constant->dest : nullptr;
}

static ir_lower_result_t
n00bb_emit_comparison_test(ir_lower_context_t *context, ir_value_t *value,
                           parse_sym_entry_t *member, ir_span_t span)
{
    if (!member || !member->type
        || !ir_type_eq(context->module, member->type, value->type))
        return lower_internal(context, span,
            "comparison member has no matching enum value and type");
    ir_value_t *constant = n00bb_emit_enum_member(context, member, span);
    ir_instr_t *test = constant
        ? ir_lower_emit_instruction(context, IR_OP_EQ, span) : nullptr;
    th_type *bool_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("bool"));
    if (!test || !bool_type
        || !ir_lower_set_temp_dest(context, test, CORE_STRING_STATIC("__n00bb_"), bool_type)
        || !ir_instr_operand_value(test, value)
        || !ir_instr_operand_value(test, constant))
        return lower_internal(context, span,
            "could not test the comparison result");
    return (ir_lower_result_t){IR_LOWER_VALUE, test->dest};
}

parse_sym_entry_t *
n00bb_comparison_member(n00bb_link_analysis_t *analysis, th_type *type,
                        core_string_t name)
{
    if (!analysis || !type || !name.data) return nullptr;
    for (size_t i = 0; i < analysis->units.len; i++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[i]);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        parse_namespace_t *type_ns = symtab
            ? parse_symtab_ns(symtab, CORE_STRING_STATIC("type")) : nullptr;
        for (parse_scope_t *scope = symtab ? symtab->all_scopes : nullptr;
             scope; scope = scope->all_next) {
            if (!type_ns || scope->ns != type_ns) continue;
            for (parse_sym_entry_t *symbol = scope->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (!symbol->type || !symbol->member_scope
                    || !core_string_eq(symbol->member_scope->scope_tag,
                                       CORE_STRING_STATIC("enum"))
                    || th_hash(state->types, symbol->type)
                        != th_hash(state->types, type)) continue;
                parse_sym_entry_t *member = parse_symtab_lookup_in(
                    symbol->member_scope, name);
                if (member && member->kind == PARSE_SYM_ENUM_CONST)
                    return member;
            }
        }
    }
    return nullptr;
}

parse_sym_entry_t *const *
n00bb_comparison_members(n00bb_link_analysis_t *analysis,
                         n00bb_comparison_t *cache, th_type *type)
{
    static const core_string_t names[] = {
        CORE_STRING_STATIC("less"), CORE_STRING_STATIC("equal"),
        CORE_STRING_STATIC("greater"), CORE_STRING_STATIC("unordered"),
    };
    if (cache->type != th_resolve(type)) {
        cache->type = th_resolve(type);
        for (size_t i = 0; i < 4; i++)
            cache->members[i] = n00bb_comparison_member(analysis, type,
                                                        names[i]);
    }
    return cache->members;
}

ir_value_t *
n00bb_emit_flagset_operator(ir_lower_context_t *context, core_string_t method,
                            th_type *result, ir_value_t *left,
                            ir_value_t *right, ir_span_t span)
{
    static const struct {
        core_string_t method;
        ir_op_t op;
    } operators[] = {
        {CORE_STRING_STATIC("__add"), IR_OP_BITSET_UNION},
        {CORE_STRING_STATIC("__bit_or"), IR_OP_BITSET_UNION},
        {CORE_STRING_STATIC("__subtract"), IR_OP_BITSET_DIFFERENCE},
        {CORE_STRING_STATIC("__bit_and"), IR_OP_BITSET_INTERSECTION},
        {CORE_STRING_STATIC("__bit_xor"), IR_OP_BITSET_XOR},
        {CORE_STRING_STATIC("__equal"), IR_OP_EQ},
    };
    for (size_t i = 0; right && i < sizeof(operators) / sizeof(*operators);
         i++) {
        if (!core_string_eq(method, operators[i].method)) continue;
        ir_instr_t *instruction = ir_lower_emit_instruction(context,
            operators[i].op, span);
        return instruction && ir_lower_set_temp_dest(context, instruction,
                CORE_STRING_STATIC("__n00bb_"), result)
            && ir_instr_operand_value(instruction, left)
            && ir_instr_operand_value(instruction, right)
            ? instruction->dest : nullptr;
    }
    return nullptr;
}

ir_lower_result_t
n00bb_emit_untyped_const(ir_lower_context_t *context, __int128 value,
                         ir_span_t span)
{
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    if (!constant
        || !ir_lower_set_temp_dest(context, constant,
               CORE_STRING_STATIC("__lowered_"),
               th_var(ir_module_types(context->module)))
        || !ir_instr_operand_int_lit(constant, (ir_lit_t){
               .neg = value < 0,
               .mag = (ir_uint_t)(value < 0 ? -value : value)}))
        return lower_internal(context, span,
                              "could not emit a constant's value");
    return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
}

static ir_value_t *
n00bb_emit_select(ir_lower_context_t *context, th_type *type,
                  ir_value_t *condition, ir_value_t *chosen,
                  ir_value_t *otherwise, ir_span_t span)
{
    ir_instr_t *select = condition && chosen && otherwise
        ? ir_lower_emit_instruction(context, IR_OP_SELECT, span)
        : nullptr;
    return select && ir_lower_set_temp_dest(context, select,
            CORE_STRING_STATIC("__n00bb_"), type)
        && ir_instr_operand_value(select, condition)
        && ir_instr_operand_value(select, chosen)
        && ir_instr_operand_value(select, otherwise)
        ? select->dest : nullptr;
}

ir_value_t *
n00bb_emit_three_way(ir_lower_context_t *context, th_type *type,
                     parse_sym_entry_t *const members[4], ir_value_t *left,
                     ir_value_t *right, ir_span_t span)
{
    th_type *truth = n00bb_ir_builtin_type(context->module,
                                           CORE_STRING_STATIC("bool"));
    ir_value_t *less = n00bb_emit_binary_ir(context, IR_OP_LT, truth, left,
                                            right, span);
    ir_value_t *equal = less ? n00bb_emit_binary_ir(context, IR_OP_EQ, truth,
                                                    left, right, span)
                             : nullptr;
    ir_value_t *after = n00bb_emit_enum_member(context, members[2], span);
    if (equal && ir_machine_of(context->module, left->type).class
            == IR_CLASS_FLOAT) {
        ir_value_t *greater = n00bb_emit_binary_ir(context, IR_OP_GT, truth,
                                                   left, right, span);
        after = greater && members[3] ? n00bb_emit_select(context, type,
            greater, after,
            n00bb_emit_enum_member(context, members[3], span), span)
            : nullptr;
    }
    ir_value_t *rest = equal && after ? n00bb_emit_select(context, type,
        equal, n00bb_emit_enum_member(context, members[1], span), after,
        span) : nullptr;
    return rest ? n00bb_emit_select(context, type, less,
        n00bb_emit_enum_member(context, members[0], span), rest, span)
        : nullptr;
}

bool
n00bb_primitive_operator(ir_class_t class, core_string_t method)
{
    bool primitive = class == IR_CLASS_SINT || class == IR_CLASS_UINT
        || class == IR_CLASS_FLOAT || class == IR_CLASS_BOOL;
    if (class == IR_CLASS_PTR)
        return core_string_eq(method, CORE_STRING_STATIC("__equal"));
    return primitive && (n00bb_binary_operator_method(method)
        || core_string_eq(method, CORE_STRING_STATIC("__equal"))
        || core_string_eq(method, CORE_STRING_STATIC("__cmp"))
        || core_string_eq(method, CORE_STRING_STATIC("__not"))
        || core_string_eq(method, CORE_STRING_STATIC("__negate")));
}

ir_value_t *
n00bb_emit_primitive(ir_lower_context_t *context, core_string_t method,
                     th_type *result, ir_value_t *left, ir_value_t *right,
                     ir_span_t span)
{
    const n00bb_binary_operator_t *binary = right
        ? n00bb_binary_operator_method(method) : nullptr;
    if (binary)
        return n00bb_emit_binary_ir(context, binary->op, result, left, right,
                                    span);
    if (right && core_string_eq(method, CORE_STRING_STATIC("__equal")))
        return n00bb_emit_binary_ir(context, IR_OP_EQ, result, left, right,
                                    span);
    if (!right && core_string_eq(method, CORE_STRING_STATIC("__not"))) {
        ir_instr_t *no = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                   span);
        return no && ir_lower_set_temp_dest(context, no,
                CORE_STRING_STATIC("__n00bb_"), left->type)
            && ir_instr_operand_int(no, 0)
            ? n00bb_emit_binary_ir(context, IR_OP_EQ, result, left,
                                   no->dest, span)
            : nullptr;
    }
    if (!right && core_string_eq(method, CORE_STRING_STATIC("__negate"))) {
        ir_instr_t *negate = ir_lower_emit_instruction(context, IR_OP_NEG,
                                                       span);
        ir_class_t class = ir_machine_of(context->module, left->type).class;
        return negate && ir_lower_set_temp_dest(context, negate,
                CORE_STRING_STATIC("__n00bb_"), result)
            && ir_instr_operand_value(negate, left)
            && ((class != IR_CLASS_SINT && class != IR_CLASS_UINT)
                || ir_instr_add_mod(negate, CORE_STRING_STATIC("trap")))
            ? negate->dest : nullptr;
    }
    return nullptr;
}

static ir_lower_result_t
n00bb_lower_comparison(ir_lower_context_t *context,
                       ir_value_t *const *operands, th_type *result_type,
                       ir_span_t span, core_string_t spelling)
{
    result_type = th_name(ir_module_types(context->module),
                          CORE_STRING_STATIC("bool"));
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    bool equality = core_string_eq(spelling, CORE_STRING_STATIC("=="))
        || core_string_eq(spelling, CORE_STRING_STATIC("!="));
    n00bb_link_analysis_t *analysis = caller
        ? caller->link_analysis : nullptr;
    th_type *comparison_type = equality ? result_type
        : th_name(ir_module_types(context->module), CORE_STRING_STATIC("Comparison"));
    if (!comparison_type)
        return lower_internal(context, span,
            "comparison expression has no comparison type");
    ir_lower_result_t called = n00bb_lower_operator_method(context,
        equality ? CORE_STRING_STATIC("__equal") : CORE_STRING_STATIC("__cmp"),
        operands, 2, comparison_type, span);
    if (called.status != IR_LOWER_VALUE) return called;
    ir_value_t *comparison = called.value;
    ir_instr_t *call = comparison->kind == IR_VAL_INSTR ? comparison->def
                                                        : nullptr;
    if (call && (call->op == IR_OP_DISPATCH || call->op == IR_OP_CALL) && lower
        && !ir_annot_set(lower->ir_context, &call->annots, (ir_annot_t){
                .key = N00BB_COMPARISON_KEY,
                .kind = IR_ANNOT_BOOL, .b = true}))
        return lower_internal(context, span,
            "could not mark the comparison call");

    if (equality) {
        ir_lower_result_t equal = {IR_LOWER_VALUE, comparison};
        if (equal.status != IR_LOWER_VALUE
            || core_string_eq(spelling, CORE_STRING_STATIC("==")))
            return equal;
        ir_instr_t *false_value = ir_lower_emit_instruction(context,
                                                             IR_OP_CONST,
                                                             span);
        if (!false_value
            || !ir_lower_set_temp_dest(context, false_value,
                                       CORE_STRING_STATIC("__n00bb_"), result_type)
            || !ir_instr_operand_int(false_value, 0))
            return lower_internal(context, span,
                "could not emit inequality's false value");
        ir_instr_t *not_equal = ir_lower_emit_instruction(context, IR_OP_EQ,
                                                           span);
        if (!not_equal
            || !ir_lower_set_temp_dest(context, not_equal,
                                       CORE_STRING_STATIC("__n00bb_"), result_type)
            || !ir_instr_operand_value(not_equal, equal.value)
            || !ir_instr_operand_value(not_equal, false_value->dest))
            return lower_internal(context, span,
                "could not negate the equality result");
        return (ir_lower_result_t){IR_LOWER_VALUE, not_equal->dest};
    }

    core_string_t first = core_string_eq(spelling, CORE_STRING_STATIC("<"))
        || core_string_eq(spelling, CORE_STRING_STATIC("<="))
            ? CORE_STRING_STATIC("less") : CORE_STRING_STATIC("greater");
    parse_sym_entry_t *first_member = n00bb_comparison_member(
        analysis, comparison_type, first);
    ir_lower_result_t test = first_member
        ? n00bb_emit_comparison_test(context, comparison, first_member, span)
        : lower_internal(context, span,
            "Comparison is missing the required ordering member");
    if (test.status != IR_LOWER_VALUE) return test;
    if (!core_string_eq(spelling, CORE_STRING_STATIC("<="))
        && !core_string_eq(spelling, CORE_STRING_STATIC(">=")))
        return test;
    parse_sym_entry_t *equal_member = n00bb_comparison_member(
        analysis, comparison_type, CORE_STRING_STATIC("equal"));
    ir_lower_result_t equal = equal_member
        ? n00bb_emit_comparison_test(context, comparison, equal_member, span)
        : lower_internal(context, span,
            "Comparison is missing the equal member");
    if (equal.status != IR_LOWER_VALUE) return equal;
    ir_instr_t *truth = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    ir_instr_t *either = ir_lower_emit_instruction(context, IR_OP_SELECT,
                                                   span);
    if (!truth || !either
        || !ir_lower_set_temp_dest(context, truth,
                                   CORE_STRING_STATIC("__n00bb_"), result_type)
        || !ir_instr_operand_int(truth, 1)
        || !ir_lower_set_temp_dest(context, either,
                                   CORE_STRING_STATIC("__n00bb_"), result_type)
        || !ir_instr_operand_value(either, test.value)
        || !ir_instr_operand_value(either, truth->dest)
        || !ir_instr_operand_value(either, equal.value))
        return lower_internal(context, span,
            "could not combine comparison outcomes");
    return (ir_lower_result_t){IR_LOWER_VALUE, either->dest};
}

ir_lower_result_t
n00bb_lower_operator_method(ir_lower_context_t *context,
                            core_string_t method,
                            ir_value_t *const *operands,
                            size_t operand_count, th_type *result_type,
                            ir_span_t span)
{
    ir_value_t *value = method.data
        ? n00bb_dispatch(context, method, operands, operand_count,
                         result_type, span)
        : nullptr;
    return value ? (ir_lower_result_t){IR_LOWER_VALUE, value}
        : lower_internal(context, span, "operator has no method to dispatch");
}

static bool
n00bb_literal_fits_machine(ir_lit_t literal, ir_machine_t machine)
{
    if (machine.width == 0 || machine.width > 128) return false;
    if (machine.class == IR_CLASS_UINT)
        return !literal.neg && (machine.width == 128
            || literal.mag < ((ir_uint_t)1 << machine.width));
    if (machine.class != IR_CLASS_SINT) return false;
    ir_uint_t limit = (ir_uint_t)1 << (machine.width - 1);
    return literal.neg ? literal.mag <= limit : literal.mag < limit;
}

static ir_value_t *
n00bb_constant_as(ir_lower_context_t *context, ir_value_t *value,
                  th_type *target, ir_span_t span)
{
    ir_instr_t *constant = value && target ? value->def : nullptr;
    if (!constant || constant->op != IR_OP_CONST || constant->n_operands != 1
        || constant->operands[0].kind != IR_OPD_CONST
        || constant->operands[0].lit.is_float
        || !n00bb_literal_fits_machine(constant->operands[0].lit.i,
                                       ir_machine_of(context->module, target)))
        return value;
    ir_instr_t *typed = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    return typed && ir_lower_set_temp_dest(context, typed,
            CORE_STRING_STATIC("__n00bb_"), target)
        && ir_instr_operand_int_lit(typed, constant->operands[0].lit.i)
        ? typed->dest : nullptr;
}

ir_value_t *
n00bb_lower_shift_count(ir_lower_context_t *context, parse_tree_t *node,
                        ir_value_t *value, th_type *receiver,
                        ir_span_t span)
{
    th_type *target = n00bb_shift_count_type(ir_module_types(context->module),
                                             receiver);
    if (!node)
        value = n00bb_constant_as(context, value, target, span);
    if (!target || !value) {
        lower_internal(context, span, "shift count has no value");
        return nullptr;
    }
    if (ir_type_eq(context->module, value->type, target)) return value;
    ir_value_t *proved = node
        ? n00bb_typed_integer_literal(context, node, target) : nullptr;
    if (proved && ir_type_eq(context->module, proved->type, target))
        return proved;

    ir_machine_t source = ir_machine_of(context->module, value->type);
    ir_machine_t destination = ir_machine_of(context->module, target);
    ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST, span);
    if (!cast || !ir_lower_set_temp_dest(context, cast, CORE_STRING_STATIC("__n00bb_"), target)
        || !ir_instr_operand_value(cast, value)
        || (source.width > destination.width
            && !ir_instr_add_mod(cast, CORE_STRING_STATIC("trap")))) {
        lower_internal(context, span,
            "could not lower the shift count conversion");
        return nullptr;
    }
    return cast->dest;
}

static bool
n00bb_nil_literal(ir_value_t *value)
{
    return value && (th_is_nil(value->type)
                     || (value->def && value->def->op == IR_OP_NIL));
}

static ir_lower_result_t
lower_incomplete(ir_lower_context_t *context, ir_span_t span,
                 core_string_t what)
{
    n00bb_lower_context_t *lower = context->user;
    if (lower && n00bb_unit_failed(lower->unit) && lower_mark_failed(context))
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    return lower_internal_string(context, span, what);
}

ir_lower_result_t
n00bb_lower_binary(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state)
{
    (void)state;
    if (operand_count != 2 || !operands || !operands[0] || !operands[1]
        || !result_type)
        return lower_incomplete(context, span,
            CORE_STRING_STATIC("binary operation has incomplete operands or type"));
    core_string_t spelling = core_tree_num_children(visit->node) > 1
        ? first_token_text(core_tree_child(visit->node, 1))
        : (core_string_t){0};
    spelling = n00bb_canonical_logical_spelling(spelling);
    const n00bb_binary_operator_t *operator =
        n00bb_binary_operator_spelled(spelling);
    ir_op_t op;
    if (operator) op = operator->op;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("==")))
        op = IR_OP_EQ;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("!=")))
        op = IR_OP_NE;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("<")))
        op = IR_OP_LT;
    else if (core_string_eq(spelling, CORE_STRING_STATIC("<=")))
        op = IR_OP_LE;
    else if (core_string_eq(spelling, CORE_STRING_STATIC(">")))
        op = IR_OP_GT;
    else if (core_string_eq(spelling, CORE_STRING_STATIC(">=")))
        op = IR_OP_GE;
    else return lower_report(context, N00B_DIAG_NOT_SUPPORTED_YET, span,
        N00B_TEXT_NOT_SUPPORTED_YET_THE_BUILT,
        N00BB_ARGS(ARG_TEXT(spelling)));

    bool comparison = op == IR_OP_EQ || op == IR_OP_NE || op == IR_OP_LT
        || op == IR_OP_LE || op == IR_OP_GT || op == IR_OP_GE;
    bool equality = op == IR_OP_EQ || op == IR_OP_NE;
    if (comparison)
        result_type = th_name(ir_module_types(context->module),
                              CORE_STRING_STATIC("bool"));
    bool left_nil = n00bb_nil_literal(operands[0]);
    bool right_nil = n00bb_nil_literal(operands[1]);
    if (equality && left_nil != right_nil) {
        ir_value_t *option = left_nil ? operands[1] : operands[0];
        if (op != IR_OP_EQ) {
            ir_value_t *present = option_present(context, option,
                                                 result_type, span);
            return present ? (ir_lower_result_t){IR_LOWER_VALUE, present}
                : lower_internal(context, span,
                                 "could not lower option inequality with nil");
        }
        ir_instr_t *test = ir_lower_emit_instruction(context,
                                                     IR_OP_OPTION_IS_NIL,
                                                     span);
        if (!test || !ir_lower_set_temp_dest(context, test,
                                             CORE_STRING_STATIC("__n00bb_"), result_type)
            || !ir_instr_operand_value(test, option))
            return lower_internal(context, span,
                               "could not lower option comparison with nil");
        return (ir_lower_result_t){IR_LOWER_VALUE, test->dest};
    }
    if (left_nil && right_nil && equality) {
        ir_instr_t *same = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
        bool is_equal = op == IR_OP_EQ;
        if (!same || !ir_lower_set_temp_dest(context, same,
                                             CORE_STRING_STATIC("__n00bb_"), result_type)
            || !ir_instr_operand_int(same, is_equal ? 1 : 0))
            return lower_internal(context, span,
                               "could not lower nil equality");
        return (ir_lower_result_t){IR_LOWER_VALUE, same->dest};
    }
    return comparison
        ? n00bb_lower_comparison(context, operands, result_type, span,
                                 spelling)
        : n00bb_lower_operator_method(context, operator->method, operands, 2,
                                      result_type, span);
}

ir_lower_result_t
n00bb_lower_nil_literal(ir_lower_context_t *context, parse_visit_t *visit,
                        ir_value_t *const *operands, size_t operand_count,
                        th_type *result_type, ir_span_t span, void *state)
{
    (void)visit;
    (void)operands;
    (void)state;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *site = tu && visit
        ? n00bb_explicit_literal_site(visit->grammar, lower->unit, visit->node)
        : nullptr;
    if (site)
        return n00bb_lower_user_literal(context, visit, site, result_type,
                                        span);
    if (operand_count)
        return lower_internal(context, span,
                              "nil literal has child values");
    result_type = th_nil(ir_module_types(context->module));
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_NIL,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), result_type))
        return lower_internal(context, span,
                           "could not emit the nil sentinel");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

ir_lower_result_t
n00bb_lower_typeof(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state)
{
    (void)visit;
    (void)state;
    result_type = th_name(ir_module_types(context->module),
                          CORE_STRING_STATIC("typespec"));
    if (operand_count != 1 || !operands || !operands[0])
        return lower_internal(context, span,
                           "typeof operand or descriptor type is incomplete");
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_TYPE_OF,
                                                        span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), result_type)
        || !ir_instr_operand_value(instruction, operands[0]))
        return lower_internal(context, span,
                           "could not emit typeof expression");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static ir_value_t *
n00bb_emit_value_instruction(ir_lower_context_t *context, ir_op_t op,
                             th_type *type, ir_value_t *left,
                             ir_value_t *right, ir_span_t span)
{
    ir_instr_t *instruction = ir_lower_emit_instruction(context, op, span);
    if (!instruction
        || !ir_lower_set_temp_dest(context, instruction, CORE_STRING_STATIC("__n00bb_"), type)
        || (left && !ir_instr_operand_value(instruction, left))
        || (right && !ir_instr_operand_value(instruction, right)))
        return nullptr;
    return instruction->dest;
}

static ir_lower_result_t
n00bb_lower_enum_from_value(ir_lower_context_t *context,
                            n00bb_lower_context_t *lower,
                            parse_sym_entry_t *enum_symbol,
                            ir_value_t *operand, th_type *target,
                            ir_span_t span)
{
    th_type *bool_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("bool"));
    ir_machine_t machine = ir_machine_of(context->module, operand->type);
    ir_block_t *join = n00bb_branch_block(context, lower, "enum_cast_join");
    ir_block_t *failure = n00bb_branch_block(context, lower,
                                              "enum_cast_failed");
    ir_instr_t *trap = failure ? ir_instr_add(failure, IR_OP_TRAP) : nullptr;
    if (!join || !failure || !trap || !ir_instr_operand_value(trap, operand)
        || !n00bb_describe_failure(lower, trap, N00BB_FAILURE_ENUM,
               (core_string_t){0}, enum_symbol->name))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the enum range check");
    trap->span = span;
    for (parse_sym_entry_t *member = enum_symbol->member_scope
             ? enum_symbol->member_scope->first_in_scope : nullptr;
         member; member = member->next_in_scope) {
        if (member->kind != PARSE_SYM_ENUM_CONST || !member->has_comptime_value
            || !member->comptime_value) continue;
        n00bb_enum_value_t *ordinal = member->comptime_value;
        ir_lit_t literal = {.mag = ordinal->magnitude, .neg = ordinal->negative};
        if (!n00bb_literal_fits_machine(literal, machine)) continue;
        ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                         span);
        if (!constant
            || !ir_lower_set_temp_dest(context, constant, CORE_STRING_STATIC("__n00bb_"),
                                       operand->type)
            || !ir_instr_operand_int_lit(constant, literal))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit the enum member value");
        ir_value_t *equal = n00bb_emit_value_instruction(context, IR_OP_EQ,
            bool_type, operand, constant->dest, span);
        ir_block_t *next = n00bb_branch_block(context, lower,
                                               "enum_cast_test");
        ir_instr_t *branch = equal && next
            ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
        if (!branch || !ir_instr_operand_value(branch, equal)
            || !ir_instr_operand_block(branch, join)
            || !ir_instr_operand_block(branch, next))
            return lower_error(context, IR_DIAG_INTERNAL, span,
                "could not emit the enum membership test");
        branch->span = span;
        context->block = next;
    }
    ir_instr_t *jump = ir_instr_add(context->block, IR_OP_JUMP);
    if (!jump || !ir_instr_operand_block(jump, failure))
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the enum range check");
    jump->span = span;
    context->block = join;
    ir_value_t *converted = n00bb_emit_value_instruction(context,
        IR_OP_ENUM_FROM_VALUE, target, operand, nullptr, span);
    if (!converted)
        return lower_error(context, IR_DIAG_INTERNAL, span,
            "could not emit the integer-to-enum conversion");
    return (ir_lower_result_t){IR_LOWER_VALUE, converted};
}

bool
foreign_pointer_type(th_type *type)
{
    th_type *resolved = th_resolve(type);
    th_type *element = th_option_item(resolved);
    if (element) resolved = th_resolve(element);
    uint8_t attrs = 0;
    return th_kind_of(resolved) == TH_INDIRECTION
        && th_ind_attrs(resolved, &attrs) && (attrs & TH_FOREIGN);
}

static ir_lower_result_t
report_undefined_cast(ir_lower_context_t *context, th_ctx *types,
                      th_type *from, th_type *target, ir_span_t span)
{
    core_string_t from_text = n00bb_print_type(types, from);
    core_string_t target_text = n00bb_print_type(types, target);
    bool references = th_kind_of(th_resolve(from)) == TH_INDIRECTION
        && th_kind_of(th_resolve(target)) == TH_INDIRECTION;
    ir_lower_result_t result = lower_report(context, N00B_DIAG_CAST_NOT_DEFINED,
        span, N00B_TEXT_CANNOT_CAST_TO_NO_CONVERSION,
        N00BB_ARGS(ARG_TEXT(from_text), ARG_TEXT(target_text)));
    if (references)
        lower_help(context, N00B_TEXT_REINTERPRETING_A_REFERENCE_NEEDS_UNSAFE,
            N00BB_NO_ARGS);
    return result;
}

static th_type *
structural_shape(n00bb_tu_state_t *state, th_type *type, bool *named,
                 th_type **object_shape)
{
    type = type ? th_resolve(type) : nullptr;
    *named = type && th_kind_of(type) == TH_NAMED;
    *object_shape = *named && n00bb_object_type(state->types, type)
        ? th_auto_deref(state->types, type) : nullptr;
    return n00bb_record_shape(state, type);
}

static bool
structural_field_fits(ir_lower_context_t *context, th_type *from,
                      th_type *to)
{
    if (ir_type_eq(context->module, from, to)) return true;
    return is_integer_type(from) && is_integer_type(to)
        && n00bb_machine_promotes(ir_machine_of(context->module, from),
                            ir_machine_of(context->module, to));
}

static ir_lower_result_t
report_tuple_cast_field(ir_lower_context_t *context, th_type *from,
                        th_type *target, core_string_t field, ir_span_t span)
{
    core_string_t from_text = lower_type_text(context, from);
    core_string_t target_text = lower_type_text(context, target);
    ir_lower_result_t result = lower_report(context, N00B_DIAG_TUPLE_CAST,
        span, N00B_TEXT_TUPLE_CAST_FIELD,
        N00BB_ARGS(ARG_TEXT(from_text), ARG_TEXT(target_text),
                   ARG_TEXT(field)));
    core_free(from_text.data);
    core_free(target_text.data);
    return result;
}

static bool
n00bb_lower_structural_cast(ir_lower_context_t *context, ir_value_t *value,
                            th_type *target, bool unsafe_cast, ir_span_t span,
                            ir_lower_result_t *result)
{
    th_type *from = value->type;
    bool from_named = false, to_named = false;
    th_type *from_object = nullptr, *to_object = nullptr;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    th_type *source = structural_shape(state, from, &from_named, &from_object);
    th_type *destination = structural_shape(state, target, &to_named,
                                            &to_object);
    th_type *resolved = th_resolve(from);
    if (destination && to_named
        && th_builtin_applied(resolved, TH_BUILTIN_TUPLE,
                              th_named_nargs(resolved))) {
        core_string_t from_text = lower_type_text(context, from);
        core_string_t target_text = lower_type_text(context, target);
        *result = lower_report(context, N00B_DIAG_TUPLE_CAST, span,
            N00B_TEXT_TUPLE_POSITIONAL_CAST,
            N00BB_ARGS(ARG_TEXT(from_text), ARG_TEXT(target_text)));
        lower_help(context, N00B_TEXT_TUPLE_POSITIONAL_CAST_HELP,
                   N00BB_NO_ARGS);
        core_free(from_text.data);
        core_free(target_text.data);
        return true;
    }
    if (!source || !destination || from_named == to_named) return false;
    if (to_named && !unsafe_cast
        && n00bb_type_has_constructor(lower->unit, target)) {
        core_string_t target_text = lower_type_text(context, target);
        *result = lower_report(context, N00B_DIAG_RECORD_HAS_CONSTRUCTOR, span,
            N00B_TEXT_TUPLE_CAST_CONSTRUCTOR,
            N00BB_ARGS(ARG_TEXT(target_text)));
        lower_help(context, N00B_TEXT_TUPLE_CONSTRUCTOR_HELP,
                   N00BB_ARGS(ARG_TEXT(target_text)));
        core_free(target_text.data);
        return true;
    }
    for (size_t i = 0; i < th_record_len(source); i++) {
        core_string_t name = th_record_member(source, i)->name;
        if (!record_lookup_text(nullptr, destination, name)) {
            *result = report_tuple_cast_field(context, from, target, name,
                                              span);
            return true;
        }
    }
    uint64_t record_hash = to_named
        ? th_hash(ir_module_types(context->module), target) : 0;
    for (size_t i = 0; i < th_record_len(destination); i++) {
        const th_member *member = th_record_member(destination, i);
        th_type *field = record_lookup_text(nullptr, source, member->name);
        if (!field && to_named
            && n00bb_union_arm_group(record_hash, member->name))
            continue;
        if (!field || !structural_field_fits(context, field, member->type)) {
            *result = report_tuple_cast_field(context, from, target,
                                              member->name, span);
            return true;
        }
    }
    if (from_object) {
        ir_value_t *view = n00bb_reference_view(context, value, span);
        value = view ? n00bb_emit_indirect_read(context, view, from_object,
                                                span) : nullptr;
    }
    ir_value_t **items = core_alloc_array(ir_value_t *,
                                          th_record_len(destination));
    bool ok = value && items;
    core_buffer_t absent = {0};
    for (size_t i = 0; ok && i < th_record_len(destination); i++) {
        const th_member *member = th_record_member(destination, i);
        th_type *given = record_lookup_text(nullptr, source, member->name);
        if (!given) {
            items[i] = n00bb_union_filler(context, member->type, span);
            core_buffer_printf(&absent, "%.*s%zu", absent.byte_len ? 1 : 0,
                               ",", i);
            ok = items[i] != nullptr;
            continue;
        }
        ir_value_t *field = n00bb_extract_step(context, value,
            (n00bb_projection_step_t){.name = member->name}, given, span);
        items[i] = field ? widened_value(context, field, member->type, span)
                         : nullptr;
        ok = items[i] != nullptr;
    }
    ir_instr_t *aggregate = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    ok = aggregate && ir_lower_set_temp_dest(context, aggregate,
        CORE_STRING_STATIC("__n00bb_cast_"), to_object ? to_object : target);
    for (size_t i = 0; ok && i < th_record_len(destination); i++)
        ok = ir_instr_operand_value(aggregate, items[i]);
    core_free(items);
    if (ok && absent.byte_len) {
        core_string_t text = ir_strdup(lower->ir_context,
                                       core_buffer_string(&absent));
        ok = text.data && ir_annot_set(lower->ir_context, &aggregate->annots,
            (ir_annot_t){.key = CORE_STRING_STATIC("n00bb.union_absent"),
                         .kind = IR_ANNOT_STR, .s = text});
    }
    core_buffer_release(&absent);
    ir_value_t *built = ok ? aggregate->dest : nullptr;
    if (built && to_object)
        built = n00bb_object_from_fields(context, built, target, span);
    *result = built ? (ir_lower_result_t){IR_LOWER_VALUE, built}
        : lower_internal(context, span,
                         "could not emit a structural tuple cast");
    return true;
}

ir_lower_result_t
n00bb_lower_cast(ir_lower_context_t *context, parse_visit_t *visit,
                 ir_value_t *const *operands, size_t operand_count,
                 th_type *result_type, ir_span_t span, void *state)
{
    (void)state;
    (void)result_type;
    parse_tree_t *spec = child_nt(visit->grammar, visit->node, CORE_STRING_STATIC("type-spec"));
    th_type *target = spec ? parse_tree_type(spec) : nullptr;
    if (operand_count != 1 || !operands || !operands[0] || !target)
        return lower_internal(context, span,
                           "cast target and expression type are incomplete");
    bool unsafe_cast = parse_tree_leaf_is(core_tree_child(visit->node, 0),
                                          CORE_STRING_STATIC("unsafe_cast"));
    n00bb_lower_context_t *lower = context->user;
    ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST, span);
    n00bb_rewrite_later(context, cast);
    if (!cast || !ir_lower_set_temp_dest(context, cast,
            CORE_STRING_STATIC("__n00bb_cast_"), target)
        || !ir_instr_operand_value(cast, operands[0])
        || !ir_annot_set(lower->ir_context, &cast->annots,
               (ir_annot_t){.key = N00BB_OPEN_CAST_KEY,
                            .kind = IR_ANNOT_BOOL, .b = unsafe_cast}))
        return lower_internal(context, span, "could not emit a cast");
    return (ir_lower_result_t){IR_LOWER_VALUE, cast->dest};
}

ir_lower_result_t
n00bb_emit_cast(ir_lower_context_t *context, parse_grammar_t *grammar,
                parse_tree_t *operand_node, ir_value_t *const *operands,
                th_type *target, bool unsafe_cast, ir_span_t span)
{
    if (ir_type_eq(context->module, operands[0]->type, target)) {
        if (operands[0]->owner == context->function)
            return (ir_lower_result_t){IR_LOWER_VALUE, operands[0]};
        ir_instr_t *copy = ir_lower_emit_instruction(context, IR_OP_COPY,
                                                     span);
        if (!copy || !ir_lower_set_temp_dest(context, copy, CORE_STRING_STATIC("__n00bb_cast_"),
                                             target)
            || !ir_instr_operand_value(copy, operands[0]))
            return lower_internal(context, span,
                               "could not emit an identity cast");
        return (ir_lower_result_t){IR_LOWER_VALUE, copy->dest};
    }
    th_ctx *types = ir_module_types(context->module);
    th_type *from = operands[0]->type;
    ir_lower_result_t structural = {0};
    if (n00bb_lower_structural_cast(context, operands[0], target, unsafe_cast,
                                    span, &structural))
        return structural;
    uint8_t source_attrs = 0, target_attrs = 0;
    bool plain_reference_cast = th_ind_attrs(from, &source_attrs)
        && th_ind_attrs(target, &target_attrs)
        && source_attrs == 0 && target_attrs == 0;
    ir_machine_t from_machine = ir_machine_of(context->module, from);
    ir_machine_t target_machine = ir_machine_of(context->module, target);
    ir_value_t *word_operand[1] = {operands[0]};
    if (unsafe_cast && foreign_pointer_type(target) && is_integer_type(from)
        && from_machine.width < 64) {
        th_type *word = th_name(types, from_machine.class == IR_CLASS_SINT
            ? CORE_STRING_STATIC("int") : CORE_STRING_STATIC("uint"));
        word_operand[0] = word ? n00bb_widen_integer_operand(context,
            operands[0], word, span) : nullptr;
        if (!word_operand[0])
            return lower_internal(context, span,
                               "could not widen the address operand");
        operands = word_operand;
        from = word_operand[0]->type;
        from_machine = ir_machine_of(context->module, from);
    }
    uint8_t plain_attrs = 0;
    if (unsafe_cast
        && ((foreign_pointer_type(from) && is_integer_type(target)
             && target_machine.width == 64)
            || (foreign_pointer_type(target) && is_integer_type(from)
                && from_machine.width == 64)
            || (foreign_pointer_type(from)
                && th_ind_attrs(target, &plain_attrs) && !plain_attrs)))
        plain_reference_cast = true;
    if (unsafe_cast && is_integer_type(from) && is_integer_type(target)) {
        ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST,
                                                     span);
        if (!cast || !ir_lower_set_temp_dest(context, cast, CORE_STRING_STATIC("__n00bb_"),
                                             target)
            || !ir_instr_operand_value(cast, operands[0])
            || (!n00bb_machine_promotes(from_machine, target_machine)
                && !ir_instr_add_mod(cast, CORE_STRING_STATIC("wrap"))))
            return lower_internal(context, span,
                               "could not emit an unchecked integer cast");
        return (ir_lower_result_t){IR_LOWER_VALUE, cast->dest};
    }
    if (unsafe_cast && !plain_reference_cast)
    {
        core_string_t from_text = lower_type_text(context, from);
        core_string_t target_text = lower_type_text(context, target);
        ir_lower_result_t result = lower_report(context,
            N00B_DIAG_UNSAFE_CAST_OPERANDS, span,
            N00B_TEXT_UNSAFE_CAST_CANNOT_CONVERT_TO,
            N00BB_ARGS(ARG_TEXT(from_text), ARG_TEXT(target_text)));
        core_free(from_text.data);
        core_free(target_text.data);
        return result;
    }
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    parse_sym_entry_t *from_flagset = nullptr;
    parse_sym_entry_t *to_flagset = nullptr;
    bool source_flagset = caller && n00bb_flagset_type(lower->unit, caller,
        from, &from_flagset);
    bool target_flagset = caller && n00bb_flagset_type(lower->unit, caller,
        target, &to_flagset);
    parse_sym_entry_t *flagset_symbol = source_flagset
        ? from_flagset : target_flagset ? to_flagset : nullptr;
    if (flagset_symbol
        && ((source_flagset && is_integer_type(target))
            || (target_flagset && is_integer_type(from)))) {
        const n00bb_flagset_info_t *info = parse_sym_binding(
            canonical_symbol(flagset_symbol),
            CORE_STRING_STATIC("n00bb.flagset-info"));
        th_type *integer_type = source_flagset ? target : from;
        ir_machine_t integer = ir_machine_of(context->module, integer_type);
        bool sufficient = info
            && ((integer.class == IR_CLASS_UINT
                    && integer.width >= info->atomic_count)
                || (integer.class == IR_CLASS_SINT
                    && integer.width > info->atomic_count));
        if (!sufficient) {
            core_string_t integer_text = lower_type_text(context,
                                                         integer_type);
            ir_lower_result_t result = lower_report(context,
                N00B_DIAG_FLAGSET_INTEGER_TOO_NARROW, span,
                N00B_TEXT_IS_TOO_NARROW_TO_HOLD,
                N00BB_ARGS(ARG_TEXT(integer_text),
                    ARG_TEXT(flagset_symbol->name),
                ARG_NUMBER(info ? (unsigned)info->atomic_count : 0u)));
            lower_help(context, N00B_TEXT_CAST_TO_AN_UNSIGNED_TYPE,
                N00BB_NO_ARGS);
            core_free(integer_text.data);
            return result;
        }
        ir_instr_t *conversion = ir_lower_emit_instruction(context,
            source_flagset ? IR_OP_BITSET_VALUE
                           : IR_OP_BITSET_FROM_VALUE, span);
        if (!conversion || !ir_lower_set_temp_dest(context, conversion,
                                                   CORE_STRING_STATIC("__n00bb_"), target)
            || !ir_instr_operand_value(conversion, operands[0])
            || !ir_instr_add_mod(conversion, CORE_STRING_STATIC("trap")))
            return lower_internal(context, span,
                               "could not emit the flagset integer conversion");
        return (ir_lower_result_t){IR_LOWER_VALUE, conversion->dest};
    }
    parse_sym_entry_t *enum_symbol = caller
        ? n00bb_enum_type_symbol(caller->link_analysis, from) : nullptr;
    if (enum_symbol && is_integer_type(target)) {
        const n00bb_enum_range_t *range = parse_sym_binding(enum_symbol,
            CORE_STRING_STATIC("n00bb.enum-range"));
        ir_machine_t destination = ir_machine_of(context->module, target);
        bool represents_range = range && (range->is_signed
            ? destination.class == IR_CLASS_SINT
                && destination.width >= range->magnitude_bits + 1
            : (destination.class == IR_CLASS_UINT
                    && destination.width >= range->magnitude_bits)
                || (destination.class == IR_CLASS_SINT
                    && destination.width > range->magnitude_bits));
        ir_instr_t *conversion = ir_lower_emit_instruction(context,
                                                            IR_OP_ENUM_VALUE,
                                                            span);
        if (!range || !conversion
            || !ir_lower_set_temp_dest(context, conversion,
                                       CORE_STRING_STATIC("__n00bb_"), target)
            || !ir_instr_operand_value(conversion, operands[0])
            || (!represents_range && !ir_instr_add_mod(conversion, CORE_STRING_STATIC("trap"))))
            return lower_internal(context, span,
                "could not emit the semantic enum-to-integer conversion");
        return (ir_lower_result_t){IR_LOWER_VALUE, conversion->dest};
    }
    parse_sym_entry_t *target_enum = caller
        ? n00bb_enum_type_symbol(caller->link_analysis, target) : nullptr;
    if (target_enum && is_integer_type(from))
        return n00bb_lower_enum_from_value(context, lower, target_enum,
                                           operands[0], target, span);
    th_type *from_definition = th_type_definition(types, from);
    th_type *to_definition = th_type_definition(types, target);
    if (unsafe_cast) {
        ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_BITCAST,
                                                     span);
        if (!cast || !ir_lower_set_temp_dest(context, cast,
                                             CORE_STRING_STATIC("__n00bb_"), target)
            || !ir_instr_operand_value(cast, operands[0])
            || ((ir_value_has_external_writable_storage(operands[0])
                 || foreign_pointer_type(from))
                && !ir_value_set_external_writable_storage(lower->ir_context,
                                                             cast->dest)))
            return lower_internal(context, span,
                               "could not preserve unsafe reference cast provenance");
        return (ir_lower_result_t){IR_LOWER_VALUE, cast->dest};
    }
    bool boxing = th_builtin_kind(types, target) == TH_BUILTIN_ANY;
    bool nominal = (from_definition
            && ir_type_eq(context->module, from_definition, target))
        || (to_definition
            && ir_type_eq(context->module, to_definition, from));
    if (boxing || nominal) {
        if (operand_node && !n00bb_attach_expression_shape(context, grammar,
                                                           operand_node,
                                                           operands[0]))
            return lower_internal(context, span,
                               "could not retain the cast operand shape");
        ir_instr_t *conversion = ir_lower_emit_instruction(context,
            boxing ? IR_OP_BOX : IR_OP_NOMINAL_CAST, span);
        if (!conversion || !ir_lower_set_temp_dest(context, conversion,
                                                   CORE_STRING_STATIC("__n00bb_"), target)
            || !ir_instr_operand_value(conversion, operands[0]))
            return lower_internal(context, span,
                               "could not retain semantic cast information");
        if (ir_value_shape(operands[0])
            && !ir_value_set_shape(conversion->dest, ir_value_shape(operands[0])))
            return lower_internal(context, span,
                               "could not retain cast array shape");
        return (ir_lower_result_t){IR_LOWER_VALUE, conversion->dest};
    }
    if (from_definition || to_definition)
        return report_undefined_cast(context, types, from, target, span);
    ir_machine_t source = ir_machine_of(context->module, operands[0]->type);
    ir_machine_t destination = ir_machine_of(context->module, target);
    bool source_integer = source.class == IR_CLASS_SINT
        || source.class == IR_CLASS_UINT;
    bool target_integer = destination.class == IR_CLASS_SINT
        || destination.class == IR_CLASS_UINT;
    bool source_float = source.class == IR_CLASS_FLOAT;
    bool target_float = destination.class == IR_CLASS_FLOAT;
    if (ir_type_eq(context->module, from, th_name(types, CORE_STRING_STATIC("byte"))))
        target_integer = is_integer_type(target);
    bool float_to_float = source_float && target_float;
    if (!(source_integer && target_integer)
        && !(source_integer && target_float)
        && !(source_float && target_integer)
        && !float_to_float)
        return report_undefined_cast(context, types, from, target, span);
    bool needs_check = (source_float && !float_to_float)
        || (source_integer && target_integer
            && !n00bb_machine_promotes(source, destination));
    core_string_t rounding = float_to_float
        ? destination.width < source.width ? CORE_STRING_STATIC("nearest")
                                           : (core_string_t){}
        : source_float ? CORE_STRING_STATIC("to_zero")
        : target_float ? CORE_STRING_STATIC("nearest") : (core_string_t){};
    ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST, span);
    if (!cast || !ir_lower_set_temp_dest(context, cast,
                                         CORE_STRING_STATIC("__n00bb_"), target)
        || !ir_instr_operand_value(cast, operands[0])
        || (needs_check && !ir_instr_add_mod(cast, CORE_STRING_STATIC("trap")))
        || (rounding.data && !ir_instr_add_mod(cast, rounding)))
        return lower_internal(context, span,
                           "could not emit a checked cast");
    return (ir_lower_result_t){IR_LOWER_VALUE, cast->dest};
}

ir_lower_result_t
n00bb_lower_bool_not(ir_lower_context_t *context, parse_visit_t *visit,
                     ir_value_t *const *operands, size_t operand_count,
                     th_type *result_type, ir_span_t span, void *state)
{
    (void)visit;
    (void)state;
    if (operand_count != 1 || !operands || !operands[0] || !result_type)
        return lower_incomplete(context, span,
            CORE_STRING_STATIC("logical negation has incomplete operands or type"));
    return n00bb_lower_operator_method(context, CORE_STRING_STATIC("__not"),
                                       operands, 1, result_type, span);
}

ir_lower_result_t
n00bb_lower_complement(ir_lower_context_t *context,
                               parse_visit_t *visit,
                               ir_value_t *const *operands,
                               size_t operand_count, th_type *result_type,
                               ir_span_t span, void *state)
{
    (void)visit;
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    if (!caller || operand_count != 1 || !operands || !operands[0]
        || !result_type)
        return lower_incomplete(context, span,
            CORE_STRING_STATIC("complement has incomplete operands or type"));
    if (!th_is_closed(ir_module_types(context->module), operands[0]->type)) {
        ir_value_t *open = n00bb_emit_temp(context, IR_OP_NOT,
                                           operands[0]->type, operands[0],
                                           span);
        return open ? (ir_lower_result_t){IR_LOWER_VALUE, open}
            : lower_internal(context, span, "could not emit complement");
    }
    if (!th_is_closed(ir_module_types(context->module), result_type))
        result_type = operands[0]->type;
    if (!ir_type_eq(context->module, operands[0]->type, result_type))
        return lower_internal(context, span,
                              "complement has incomplete operands or type");
    bool flagset = n00bb_flagset_type(lower->unit, caller,
                                     operands[0]->type, nullptr);
    ir_instr_t *instruction = ir_lower_emit_instruction(context, flagset
        ? IR_OP_BITSET_COMPLEMENT : IR_OP_NOT, span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), result_type)
        || !ir_instr_operand_value(instruction, operands[0]))
        return lower_internal(context, span,
                           "could not emit complement");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

ir_lower_result_t
n00bb_lower_negate(ir_lower_context_t *context, parse_visit_t *visit,
                   ir_value_t *const *operands, size_t operand_count,
                   th_type *result_type, ir_span_t span, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    bool negative = false;
    bool saw_sign = false;
    ir_lit_t magnitude = {0};
    parse_tree_t *literal = lower && lower->grammar && visit
        ? n00bb_simple_switch_literal(lower->grammar, visit->node,
                                      &negative, &saw_sign) : nullptr;
    bool folded_literal = literal && negative && saw_sign
        && !literal_modifier(lower->grammar, literal).data
        && n00bb_parse_switch_integer(first_token_text(literal), &magnitude);
    const ir_instr_t *folded = folded_literal && operand_count == 1
        && operands && operands[0] && operands[0]->kind == IR_VAL_INSTR
        ? operands[0]->def : nullptr;
    folded_literal = folded && folded->op == IR_OP_CONST
        && folded->n_operands == 1
        && folded->operands[0].kind == IR_OPD_CONST
        && folded->operands[0].lit.i.neg
            == (!magnitude.neg && magnitude.mag != 0);
    if (folded_literal && operand_count == 1 && operands && operands[0]) {
        if (operands[0]->kind == IR_VAL_INSTR && operands[0]->def)
            operands[0]->def->span = span;
        return (ir_lower_result_t){IR_LOWER_VALUE, operands[0]};
    }
    if (operand_count != 1 || !operands || !operands[0] || !result_type)
        return lower_incomplete(context, span,
            CORE_STRING_STATIC("unary negation has incomplete operands or type"));
    return n00bb_lower_operator_method(context, CORE_STRING_STATIC("__negate"),
                                       operands, 1, result_type, span);
}

bool
n00bb_skip_i128_min_magnitude(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering,
                              void *state)
{
    (void)state;
    n00bb_emit_t emit;
    if (entering || !visit->node
        || !n00bb_node_emit(visit->grammar, visit->node, &emit)
        || !core_string_eq(emit.action, CORE_STRING_STATIC("literal")))
        return true;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!lower || !lower->grammar) return true;
    bool negative = false;
    bool saw_sign = false;
    ir_lit_t magnitude = {0};
    parse_tree_t *literal = nullptr;
    for (parse_tree_t *ancestor = parse_tree_parent(visit->node);
         ancestor && !literal;
         ancestor = parse_tree_parent(ancestor)) {
        if (!nt_is(lower->grammar, ancestor, CORE_STRING_STATIC("unary-expr"))) continue;
        bool candidate_negative = false;
        bool candidate_saw_sign = false;
        parse_tree_t *candidate = n00bb_simple_switch_literal(
            lower->grammar, ancestor, &candidate_negative,
            &candidate_saw_sign);
        if (!candidate || !candidate_saw_sign) continue;
        literal = candidate;
        negative = candidate_negative;
        saw_sign = candidate_saw_sign;
        break;
    }
    if (!literal || !negative || !saw_sign
        || literal_modifier(lower->grammar, literal).data
        || !n00bb_parse_switch_integer(first_token_text(literal), &magnitude))
        return true;
    th_type *type = n00bb_node_type(context, visit->node);
    if (!type) return true;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    if (!constant || !ir_lower_set_temp_dest(context, constant, CORE_STRING_STATIC("__n00bb_"),
                                              type)
        || !ir_instr_operand_int_lit(constant, (ir_lit_t){
            .mag = magnitude.mag, .neg = !magnitude.neg && magnitude.mag}))
        return false;
    return ir_lower_set_node_value(context, visit->node, constant->dest);
}

static parse_sym_entry_t *
referenced_function(ir_lower_context_t *context, parse_visit_t *visit,
                    parse_tree_t **operand_out)
{
    parse_tree_t *operand = nullptr;
    for (size_t i = 0; !operand && i < core_tree_num_children(visit->node);
         i++) {
        parse_tree_t *child = core_tree_child(visit->node, i);
        if (!core_tree_is_leaf(child)) operand = child;
    }
    *operand_out = operand;
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *target = operand && lower
        ? attached_symbol(visit->grammar, lower->unit, operand) : nullptr;
    if (!target || target->kind != PARSE_SYM_FUNCTION) return nullptr;
    if (names_symbol_directly(operand)) return target;
    parse_tree_t *member = member_access_node(visit->grammar, operand);
    parse_tree_t *receiver = member && core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    return receiver && call_receiver_is_module(visit->grammar, lower->unit,
                                               receiver)
        ? target : nullptr;
}

bool
n00bb_lower_operator_annotation(ir_lower_context_t *context,
                                parse_visit_t *visit, bool entering,
                                void *state)
{
    (void)state;
    bool is_ref = core_string_eq(visit->annot->op_kind,
                                 CORE_STRING_STATIC("ref"));
    if (is_ref) {
        if (entering) {
            parse_tree_t *operand = nullptr;
            bool function_reference =
                referenced_function(context, visit, &operand) != nullptr;
            if (!operand || (function_reference
                    ? !ir_lower_skip_value(context, visit->node, operand)
                    : !n00bb_mark_reference_target(context,
                        visit->grammar, visit->node, operand)))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL,
                    lower_span_for_tree(context, visit->node),
                    "could not mark a reference target as an lvalue");
            return true;
        }

        parse_tree_t *operand = nullptr;
        n00bb_lower_context_t *lower = context->user;
        ir_span_t span = lower_span_for_tree(context, visit->node);
        parse_sym_entry_t *target = referenced_function(context, visit,
                                                        &operand);
        bool function_reference = target != nullptr;
        ir_value_t *storage = nullptr;
        if (function_reference) {
            parse_sym_entry_t *function_target = canonical_symbol(target);
            n00bb_link_analysis_t *analysis = tu_state(lower->unit)
                ? tu_state(lower->unit)->link_analysis : nullptr;
            core_translation_unit_t *owner = unit_for_symbol(
                visit->grammar, lower->units, function_target);
            n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
            ir_func_t *function = analysis
                ? link_ir_function_for_symbol(analysis, function_target)
                : nullptr;
            ir_module_t *module = owner_state
                ? unit_module(owner_state)
                : nullptr;
            if (function && ir_func_is_always_inline(function)) {
                lower_report(context, N00B_DIAG_INLINE_FUNCTION_REFERENCE,
                    span, N00B_TEXT_INLINE_FUNCTION_REFERENCE,
                    N00BB_ARGS(ARG_TEXT(function_target->name)));
                lower_help(context, N00B_TEXT_REMOVE_INLINE_OR_CALL,
                    N00BB_ARGS(ARG_TEXT(function_target->name)));
                return false;
            }
            storage = module && function
                ? ir_func_reference(module, function) : nullptr;
        }
        else if (operand && lower)
            storage = n00bb_addressable_expression_address(context,
                visit->grammar, lower->unit, operand, span);
        ir_value_t *root = storage;
        while (root && root->def && (root->def->op == IR_OP_FIELD
                                     || root->def->op == IR_OP_INDEX)
               && root->def->n_operands)
            root = ir_operand_value(&root->def->operands[0]);
        bool escapes = storage && n00bb_from_reference_address(storage)
            && !n00bb_call_argument(visit->grammar, visit->node);
        if (escapes && root && root->def && root->def->op == IR_OP_DISPATCH) {
            if (!ir_annot_set(lower->ir_context, &root->def->annots,
                    (ir_annot_t){.key = N00BB_REFERENCE_ESCAPES_KEY,
                                 .kind = IR_ANNOT_STR,
                                 .s = ir_strdup(lower->ir_context,
                                     source_text_of(lower->unit, operand))}))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                    "could not defer the reference escape check");
            escapes = false;
        }
        if (escapes) {
            lower_report(context, N00B_DIAG_REFERENCE_ADDRESS_ESCAPES, span,
                N00B_TEXT_REFERENCE_ADDRESS_ESCAPES,
                N00BB_ARGS(ARG_TEXT(source_text_of(lower->unit, operand))));
            lower_help(context, N00B_TEXT_PASS_THE_REFERENCE_DIRECTLY,
                       N00BB_NO_ARGS);
            return false;
        }
        if (storage && storage->def && storage->def->op == IR_OP_INDEX
            && storage->def->n_operands
            && storage->def->operands[0].kind == IR_OPD_VALUE) {
            ir_shape_t *shape = ir_value_shape(
                storage->def->operands[0].value);
            if (!shape) {
                parse_sym_entry_t *root = n00bb_assignment_root_symbol(
                    visit->grammar, lower->unit, operand);
                shape = root ? parse_sym_binding(root,
                    CORE_STRING_STATIC("ir.array-shape")) : nullptr;
            }
            ir_shape_storage_t layout = {0};
            if (shape && ir_shape_storage(shape, &layout)
                && layout.classification_known
                && layout.flat_prefix_rank > 1)
                return (lower_report(context, N00B_DIAG_NOT_SUPPORTED_YET, span,
                    N00B_TEXT_NOT_SUPPORTED_YET_REF_TO,
                    N00BB_NO_ARGS), false);
            if (!ir_annot_set(lower->ir_context, &storage->def->annots,
                    (ir_annot_t){
                        .key = CORE_STRING_STATIC("n00bb.array.direct_ref"),
                        .kind = IR_ANNOT_BOOL,
                        .b = true,
                    }))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL,
                    span, "could not mark direct Array element reference");
        }
        if (!storage) {
            core_string_t text = operand
                ? source_text_of(lower ? lower->unit : nullptr, operand)
                : (core_string_t){0};
            lower_report(context, N00B_DIAG_REF_NOT_ADDRESSABLE,
                operand ? lower_span_for_tree(context, operand) : span,
                N00B_TEXT_REF_NEEDS_A_VARIABLE_FIELD,
                N00BB_ARGS(ARG_TEXT(text.u8_bytes ? text
                                    : n00bb_text_form(N00B_TEXT_THIS))));
            lower_help(context, N00B_TEXT_STORE_THE_VALUE_IN_A_3,
                N00BB_NO_ARGS);
            return false;
        }
        ir_value_t *referenced = storage;
        if (!function_reference) {
            ir_instr_t *open = ir_lower_emit_instruction(context,
                IR_OP_DISPATCH, span);
            referenced = open && ir_lower_set_temp_dest(context, open,
                    CORE_STRING_STATIC("__n00bb_ref_"),
                    th_var(ir_module_types(context->module)))
                && ir_instr_operand_name(open, N00BB_REF_METHOD)
                && ir_instr_operand_fresh(open, storage)
                ? open->dest : nullptr;
            if (referenced) n00bb_rewrite_later(context, open);
        }
        if (!referenced
            || !ir_lower_set_node_value(context, visit->node, referenced)) {
            n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                                 "could not retain the reference value");
            return false;
        }
        return true;
    }
    if (!core_string_eq(visit->annot->op_kind,
                        CORE_STRING_STATIC("unwrap_result"))) {
        if (!entering) return true;
        lower_report(context, N00B_DIAG_NOT_SUPPORTED_YET,
            lower_span_for_tree(context, visit->node),
            N00B_TEXT_NOT_SUPPORTED_YET_THE_OPERATOR,
            N00BB_ARGS(ARG_TEXT(visit->annot->op_kind)));
        return false;
    }
    if (entering) return true;

    ir_span_t span = lower_span_for_tree(context, visit->node);
    parse_tree_t *operand_node = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    ir_value_t *outcome = operand_node
        ? ir_lower_node_value(context, operand_node) : nullptr;
    ir_value_t *value = nullptr;
    if (!n00bb_unwrap_call_outcome(context, visit, outcome, span, &value,
                                   false))
        return false;
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *callee = operand_node
        && core_tree_num_children(operand_node)
            ? core_tree_child(operand_node, 0) : nullptr;
    parse_sym_entry_t *callee_symbol = lower && callee
        ? attached_symbol(visit->grammar, lower->unit, callee) : nullptr;
    ir_shape_t *return_shape = callee_symbol
        ? parse_sym_binding(callee_symbol,
            CORE_STRING_STATIC("ir.return-shape")) : nullptr;
    if (value && return_shape
        && (!n00bb_emit_sequence_shape_check(context, value, return_shape,
                                              span)
            || !ir_value_set_shape(value, return_shape)))
        value = nullptr;
    for (size_t i = 0; value && lower
            && i < lower->pending_call_copyouts.len; i++) {
        n00bb_pending_copyouts_t *pending =
            &lower->pending_call_copyouts.data[i];
        if (pending->outcome != outcome) continue;
        for (size_t c = 0; value && c < pending->items.len; c++)
            value = n00bb_emit_call_copyout(context,
                &pending->items.data[c], value, span)
                ? value : nullptr;
        core_list_free(pending->items);
        pending->items = core_list_new(n00bb_copyout_t);
        pending->outcome = nullptr;
        break;
    }
    if (!value || !ir_lower_set_node_value(context, visit->node, value))
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                                    "could not extract the successful call value");
    return true;
}

ir_value_t *
n00bb_comparison_is(ir_lower_context_t *context, ir_value_t *left,
                    ir_value_t *right, core_string_t member, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    th_type *comparison = th_name(ir_module_types(context->module),
                                  CORE_STRING_STATIC("Comparison"));
    ir_value_t *operands[] = {left, right};
    ir_value_t *ordered = n00bb_dispatch(context, CORE_STRING_STATIC("__cmp"),
                                         operands, 2, comparison, span);
    parse_sym_entry_t *wanted = ordered && caller
        ? n00bb_comparison_member(caller->link_analysis, comparison, member)
        : nullptr;
    ir_lower_result_t test = wanted
        ? n00bb_emit_comparison_test(context, ordered, wanted, span)
        : (ir_lower_result_t){0};
    return test.status == IR_LOWER_VALUE ? test.value : nullptr;
}
