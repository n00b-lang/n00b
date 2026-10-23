#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "specialize.h"
#include "cached_body.h"
#include "compiler_tu.h"
#include "ir/doms.h"
#include "ir/operands.h"
#include "ir/pass.h"
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
} specialization_t;
core_list_decl(specialization_t);

static bool match_type(th_ctx *types, th_type *pattern, th_type *actual,
                       core_list_t(type_binding_t) *bindings)
{
    if (!pattern || !actual) return false;
    pattern = th_resolve(pattern);
    actual = th_resolve(actual);
    th_kind kind = th_kind_of(pattern);
    if (kind == TH_VAR) {
        if (!th_is_closed(types, actual)) return false;
        for (size_t i = 0; i < bindings->len; i++)
            if (bindings->data[i].variable == pattern)
                return th_hash(types, bindings->data[i].actual)
                    == th_hash(types, actual);
        core_list_push(*bindings, ((type_binding_t){pattern, actual}));
        return true;
    }
    if (kind != th_kind_of(actual)) return false;
    switch (kind) {
    case TH_NAMED: {
        core_string_t left = {}, right = {};
        size_t left_count = 0, right_count = 0;
        bool left_variant = false, right_variant = false;
        if (!th_named_info(pattern, &left, &left_variant, &left_count)
            || !th_named_info(actual, &right, &right_variant, &right_count)
            || !core_string_eq(left, right)
            || left_variant != right_variant || left_count != right_count)
            return false;
        for (size_t i = 0; i < left_count; i++) {
            const th_arg *a = th_named_arg(pattern, i);
            const th_arg *b = th_named_arg(actual, i);
            if (!a || !b || a->kind != b->kind) return false;
            switch (a->kind) {
            case TH_ARG_TYPE:
                if (!match_type(types, a->type, b->type, bindings))
                    return false;
                break;
            case TH_ARG_UINT:
                if (a->u != b->u) return false;
                break;
            case TH_ARG_SINT:
                if (a->s != b->s) return false;
                break;
            case TH_ARG_STR:
                if (!core_string_eq(a->str, b->str)) return false;
                break;
            }
        }
        return true;
    }
    case TH_INDIRECTION: {
        uint8_t left = 0, right = 0;
        return th_ind_attrs(pattern, &left)
            && th_ind_attrs(actual, &right) && left == right
            && match_type(types, th_ind_referent(pattern),
                th_ind_referent(actual), bindings);
    }
    case TH_SEQUENCE: {
        uint64_t left = 0, right = 0;
        bool left_sized = th_seq_extent(pattern, &left);
        bool right_sized = th_seq_extent(actual, &right);
        return left_sized == right_sized
            && (!left_sized || left == right)
            && match_type(types, th_seq_elem(pattern),
                th_seq_elem(actual), bindings);
    }
    case TH_FUNCTION: {
        if (th_func_nparams(pattern) != th_func_nparams(actual)
            || th_func_nkw(pattern) != th_func_nkw(actual)
            || !match_type(types, th_func_ret(pattern),
                th_func_ret(actual), bindings)) return false;
        th_type *left_error = th_func_error(pattern);
        th_type *right_error = th_func_error(actual);
        if (!!left_error != !!right_error
            || (left_error && !match_type(types, left_error,
                right_error, bindings))) return false;
        for (size_t i = 0; i < th_func_nparams(pattern); i++) {
            const th_param *a = th_func_param(pattern, i);
            const th_param *b = th_func_param(actual, i);
            if (!a || !b || a->mode != b->mode
                || !match_type(types, a->type, b->type, bindings))
                return false;
        }
        const th_param *a_rest = th_func_rest(pattern);
        const th_param *b_rest = th_func_rest(actual);
        if (!!a_rest != !!b_rest
            || (a_rest && (a_rest->mode != b_rest->mode
                || !match_type(types, a_rest->type, b_rest->type,
                    bindings)))) return false;
        for (size_t i = 0; i < th_func_nkw(pattern); i++) {
            const th_kwparam *a = th_func_kw(pattern, i);
            const th_kwparam *b = th_func_kw(actual, i);
            if (!a || !b || !core_string_eq(a->name, b->name)
                || a->mode != b->mode
                || !match_type(types, a->type, b->type, bindings))
                return false;
        }
        return true;
    }
    case TH_SUM: {
        th_comb left, right;
        if (!th_sum_comb(pattern, &left) || !th_sum_comb(actual, &right)
            || left != right || th_sum_len(pattern) != th_sum_len(actual))
            return false;
        for (size_t i = 0; i < th_sum_len(pattern); i++)
            if (!match_type(types, th_sum_arm(pattern, i),
                th_sum_arm(actual, i), bindings)) return false;
        return true;
    }
    case TH_RECORD:
        if (th_record_len(pattern) != th_record_len(actual)) return false;
        for (size_t i = 0; i < th_record_len(pattern); i++) {
            const th_member *a = th_record_member(pattern, i);
            const th_member *b = th_record_member(actual, i);
            if (!a || !b || !core_string_eq(a->name, b->name)
                || !match_type(types, a->type, b->type, bindings))
                return false;
        }
        return true;
    default:
        return th_hash(types, pattern) == th_hash(types, actual);
    }
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
    core_string_t before = n00bb_print_type(types, earlier->signature);
    core_string_t after = n00bb_print_type(types, signature);
    core_buffer_printf(&report->message,
        "'%.*s' calls itself with a type that grows on every call: its "
        "instance %.*s makes this call, which needs %.*s, so no finite set "
        "of instances exists; a recursive call in a generic function must "
        "not wrap its own type arguments",
        (int)target->name.u8_bytes, target->name.data,
        (int)before.u8_bytes, before.data,
        (int)after.u8_bytes, after.data);
    report->module = ir_module_name(target->owner);
    report->span = call->span;
    report->located = true;
}

static bool has_callback_operation(const ir_func_t *function)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        const ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_op_t op = block->instrs[i]->op;
            if (op == IR_OP_CALLBACK_CREATE || op == IR_OP_CALLBACK_INVOKE)
                return true;
        }
    }
    return false;
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

static bool
dynamic_subject(th_ctx *types, th_type *subject)
{
    th_type *resolved = th_resolve(subject);
    th_kind  kind     = th_kind_of(resolved);
    return kind == TH_TRAIT || kind == TH_SUM || kind == TH_VAR
        || th_builtin_kind(types, resolved) == TH_BUILTIN_ANY;
}

static int
static_type_test_arm(const ir_instr_t *branch, th_ctx *types)
{
    if (!branch || branch->op != IR_OP_BRANCH || branch->n_operands != 3
        || branch->operands[0].kind != IR_OPD_VALUE) return -1;
    ir_value_t *condition = branch->operands[0].value;
    ir_instr_t *test = condition && condition->kind == IR_VAL_INSTR
        ? condition->def : nullptr;
    if (!test || test->op != IR_OP_TYPE_TEST || test->n_operands != 2
        || test->operands[0].kind != IR_OPD_VALUE
        || test->operands[1].kind != IR_OPD_TYPE
        || !test->operands[0].value) return -1;
    th_type *subject = call_operand_type(test->operands[0].value, types);
    if (!subject || !th_is_closed(types, subject)
        || dynamic_subject(types, subject)) return -1;
    return th_env_type_pattern_accepts(types, test->operands[1].type, subject)
        ? 0 : 1;
}

static bool *
fold_static_type_tests(ir_func_t *function, th_ctx *types)
{
    uint32_t count = function->n_blocks;
    bool *feasible = core_alloc_array(bool, count ? count : 1);
    uint32_t *stack = core_alloc_array(uint32_t, count ? count : 1);
    ir_block_t *entry = ir_block_entry(function);
    if (!feasible || !stack || !entry || entry->index >= count) {
        core_free(stack);
        if (feasible)
            for (uint32_t b = 0; b < count; b++) feasible[b] = true;
        return feasible;
    }
    uint32_t depth = 0;
    stack[depth++] = entry->index;
    feasible[entry->index] = true;
    while (depth) {
        ir_block_t *block = function->blocks[stack[--depth]];
        ir_instr_t *term = ir_block_terminator(block);
        int fixed = static_type_test_arm(term, types);
        if (fixed >= 0) ir_branch_fold(term, (uint32_t)fixed);
        uint32_t successors = term ? ir_succ_count(term) : 0;
        for (uint32_t s = 0; s < successors; s++) {
            ir_block_t *to = ir_succ_at(term, s);
            if (!to || to->index >= count || feasible[to->index]) continue;
            feasible[to->index] = true;
            stack[depth++] = to->index;
        }
    }
    core_free(stack);
    return feasible;
}

static bool
primitive_operand_class(ir_class_t class)
{
    return class == IR_CLASS_SINT || class == IR_CLASS_UINT
        || class == IR_CLASS_FLOAT;
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
inserted_binary(ir_instr_t *before, ir_op_t op, th_type *type,
                ir_value_t *left, ir_value_t *right)
{
    ir_value_t *value = inserted_value(before, op, type);
    return value && ir_instr_operand_value(value->def, left)
        && ir_instr_operand_value(value->def, right) ? value : nullptr;
}

static ir_value_t *
comparison_member(ir_instr_t *before, th_type *type, core_string_t name,
                  uint64_t ordinal)
{
    ir_value_t *value = inserted_value(before, IR_OP_ENUM_CONST, type);
    return value && ir_instr_operand_name(value->def, name)
        && ir_instr_operand_int_lit(value->def,
               (ir_lit_t){.mag = ordinal}) ? value : nullptr;
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
integer_arguments_as(ir_func_t *function, ir_instr_t *call, ir_func_t *target)
{
    ir_module_t *module = function->owner;
    size_t count = th_func_nparams(target->type);
    if (call->n_operands != count + 1) return true;
    for (size_t i = 0; i < count; i++) {
        const th_param *formal = th_func_param(target->type, i);
        ir_value_t *argument = ir_operand_value(&call->operands[i + 1]);
        if (!formal || !argument || ir_type_eq(module, argument->type,
                                               formal->type)) continue;
        ir_class_t source = ir_machine_of(module, argument->type).class;
        ir_class_t wanted = ir_machine_of(module, formal->type).class;
        if ((source != IR_CLASS_SINT && source != IR_CLASS_UINT)
            || (wanted != IR_CLASS_SINT && wanted != IR_CLASS_UINT)) continue;
        ir_value_t *converted = operand_as(function, call, argument,
                                           formal->type);
        if (!converted) return false;
        call->operands[i + 1].value = converted;
    }
    return true;
}

static ir_value_t *
shift_count_as(ir_func_t *function, ir_instr_t *before, ir_value_t *value,
               th_type *count)
{
    ir_module_t *module = function->owner;
    if (ir_type_eq(module, value->type, count)) return value;
    ir_value_t *cast = inserted_value(before, IR_OP_CAST, count);
    bool narrowing = ir_machine_of(module, value->type).width
        > ir_machine_of(module, count).width;
    return cast && ir_instr_operand_value(cast->def, value)
        && (!narrowing || ir_instr_add_mod(cast->def,
                                           CORE_STRING_STATIC("trap")))
        ? cast : nullptr;
}

static bool
become(ir_instr_t *call, ir_op_t op, ir_value_t *first, ir_value_t *second,
       ir_value_t *third)
{
    call->op = op;
    call->n_operands = 0;
    call->n_mods = 0;
    return ir_instr_operand_value(call, first)
        && (!second || ir_instr_operand_value(call, second))
        && (!third || ir_instr_operand_value(call, third));
}

static bool
rewrite_primitive_operator(ir_func_t *function, ir_instr_t *call)
{
    ir_module_t *module = function->owner;
    ir_value_t *callee = call->op == IR_OP_CALL && call->n_operands == 3
        ? ir_operand_value(&call->operands[0]) : nullptr;
    ir_instr_t *selection = callee ? callee->def : nullptr;
    if (!selection || selection->op != IR_OP_TRAIT_METHOD
        || selection->n_operands < 2
        || selection->operands[1].kind != IR_OPD_NAME || !call->dest)
        return false;
    const n00bb_binary_operator_t *operator =
        n00bb_binary_operator_method(selection->operands[1].name);
    ir_value_t *left = ir_operand_value(&call->operands[1]);
    ir_value_t *right = ir_operand_value(&call->operands[2]);
    if (!operator || !left || !right) return false;
    th_type *result = call->dest->type;
    ir_class_t class = ir_machine_of(module, result).class;
    if (!primitive_operand_class(class)
        || ((operator->unsigned_only || operator->shift)
            && !n00bb_bitwise_type(result))) return false;
    if (operator->shift) {
        th_type *width = n00bb_integer_type_promotes_to(left->type, result)
            ? result : left->type;
        th_type *count = n00bb_bitwise_type(width)
            ? n00bb_shift_count_type(ir_module_types(module), width)
            : nullptr;
        if (!count || !n00bb_unsigned_shift_count_type(right->type))
            return false;
        left = operand_as(function, call, left, width);
        right = left ? shift_count_as(function, call, right, count) : nullptr;
        if (!right) return false;
        if (!ir_type_eq(module, width, result)) {
            ir_value_t *shifted = inserted_binary(call, operator->op, width,
                                                  left, right);
            return shifted
                && ir_instr_add_mod(shifted->def, CORE_STRING_STATIC("trap"))
                && become(call, IR_OP_CAST, shifted, nullptr, nullptr)
                && (n00bb_integer_type_promotes_to(width, result)
                    || ir_instr_add_mod(call, CORE_STRING_STATIC("trap")));
        }
    }
    bool same = operator->shift
        || (ir_type_eq(module, left->type, result)
            && ir_type_eq(module, right->type, result));
    if (!same && class == IR_CLASS_FLOAT) return false;
    if (!same) {
        left = operand_as(function, call, left, result);
        right = left ? operand_as(function, call, right, result) : nullptr;
        if (!right) return false;
    }
    if (!become(call, operator->op, left, right, nullptr)) return false;
    bool integer = class != IR_CLASS_FLOAT;
    if (integer && ir_op_accepts_mod(call->op, CORE_STRING_STATIC("trap"))
        && !ir_instr_add_mod(call, CORE_STRING_STATIC("trap"))) return false;
    if (class == IR_CLASS_SINT
        && (call->op == IR_OP_DIV || call->op == IR_OP_REM)
        && !ir_instr_add_mod(call, CORE_STRING_STATIC("trap"))) return false;
    return true;
}

static bool
rewrite_primitive_comparison(ir_func_t *function, ir_instr_t *call,
                             core_string_t method)
{
    ir_module_t *module = function->owner;
    ir_value_t *left = ir_operand_value(&call->operands[1]);
    ir_value_t *right = ir_operand_value(&call->operands[2]);
    if (!left || !right || !call->dest
        || !ir_type_eq(module, left->type, right->type)) return false;
    ir_class_t class = ir_machine_of(module, left->type).class;
    if (class != IR_CLASS_SINT && class != IR_CLASS_UINT
        && class != IR_CLASS_FLOAT && class != IR_CLASS_BOOL
        && class != IR_CLASS_PTR) return false;
    if (core_string_eq(method, CORE_STRING_STATIC("__equal")))
        return become(call, IR_OP_EQ, left, right, nullptr);
    if (!core_string_eq(method, CORE_STRING_STATIC("__cmp"))
        || class == IR_CLASS_BOOL || class == IR_CLASS_PTR) return false;
    th_ctx *types = ir_module_types(module);
    th_type *boolean = th_name(types, CORE_STRING_STATIC("bool"));
    th_type *comparison = call->dest->type;
    ir_value_t *less = inserted_binary(call, IR_OP_LT, boolean, left, right);
    ir_value_t *greater = inserted_binary(call, IR_OP_GT, boolean, left,
                                          right);
    ir_value_t *less_member = comparison_member(call, comparison,
        CORE_STRING_STATIC("less"), 0);
    ir_value_t *equal_member = comparison_member(call, comparison,
        CORE_STRING_STATIC("equal"), 1);
    ir_value_t *greater_member = comparison_member(call, comparison,
        CORE_STRING_STATIC("greater"), 2);
    if (!less || !greater || !less_member || !equal_member
        || !greater_member) return false;
    ir_value_t *level = equal_member;
    if (class == IR_CLASS_FLOAT) {
        ir_value_t *equal = inserted_binary(call, IR_OP_EQ, boolean, left,
                                            right);
        ir_value_t *unordered = comparison_member(call, comparison,
            CORE_STRING_STATIC("unordered"), 3);
        level = equal && unordered
            ? inserted_value(call, IR_OP_SELECT, comparison) : nullptr;
        if (!level
            || !become(level->def, IR_OP_SELECT, equal, equal_member,
                       unordered)) return false;
    }
    ir_value_t *upper = inserted_value(call, IR_OP_SELECT, comparison);
    return upper
        && become(upper->def, IR_OP_SELECT, greater, greater_member, level)
        && become(call, IR_OP_SELECT, less, less_member, upper);
}

typedef core_list_t(n00bb_ir_func_ptr_t) function_list_t;

typedef struct {
    const function_list_t *functions;
    core_dict_t            by_name;
    size_t                 indexed;
} method_index_t;

static const function_list_t *
methods_named(method_index_t *index, core_string_t name)
{
    for (; index->indexed < index->functions->len; index->indexed++) {
        ir_func_t *candidate = index->functions->data[index->indexed];
        if (!candidate) continue;
        function_list_t *named = core_dict_get(&index->by_name,
                                               &candidate->name, nullptr);
        if (!named) {
            named = core_alloc(function_list_t);
            *named = core_list_new(n00bb_ir_func_ptr_t);
            core_dict_put(&index->by_name, &candidate->name, named);
        }
        core_list_push(*named, candidate);
    }
    return core_dict_get(&index->by_name, &name, nullptr);
}

static void
method_index_free(method_index_t *index)
{
    core_dict_foreach(&index->by_name, item) {
        function_list_t *named = item->value;
        core_list_free(*named);
        core_free(named);
    }
    core_dict_free(&index->by_name);
}

static ir_func_t *
trait_implementation(method_index_t *methods, core_string_t method,
                     th_type *receiver, th_ctx *types)
{
    ir_func_t *generic = nullptr;
    const function_list_t *named = methods_named(methods, method);
    for (size_t i = 0; named && i < named->len; i++) {
        ir_func_t *candidate = named->data[i];
        ir_func_t *body = n00bb_cached_body(candidate);
        if (!body || !body->n_blocks) continue;
        th_ctx *candidate_types = ir_module_types(candidate->owner);
        const th_param *self = th_func_param(candidate->type, 0);
        if (!self) continue;
        if (th_hash(candidate_types, self->type) == th_hash(types, receiver))
            return candidate;
        core_list_t(type_binding_t) bindings = {0};
        if (!generic && !th_is_closed(candidate_types, self->type)
            && match_type(candidate_types, self->type, receiver, &bindings))
            generic = candidate;
        core_list_free(bindings);
    }
    return generic;
}

static bool
resolve_trait_operator(ir_func_t *function, ir_instr_t *call,
                       method_index_t *methods)
{
    ir_value_t *callee = call->op == IR_OP_CALL && call->n_operands == 3
        ? ir_operand_value(&call->operands[0]) : nullptr;
    ir_instr_t *selection = callee ? callee->def : nullptr;
    if (!selection || selection->op != IR_OP_TRAIT_METHOD
        || selection->n_operands < 3
        || selection->operands[0].kind != IR_OPD_NAME
        || selection->operands[1].kind != IR_OPD_NAME) return false;
    if (rewrite_primitive_operator(function, call)) return true;
    ir_value_t *receiver = ir_operand_value(&call->operands[1]);
    th_ctx *types = ir_module_types(function->owner);
    if (!receiver || !receiver->type || !th_is_closed(types, receiver->type))
        return false;
    core_string_t trait = selection->operands[0].name;
    core_string_t method = selection->operands[1].name;
    th_env_method_t resolved = {0};
    bool builtin = th_env_method_resolve(types, receiver->type, trait, method,
                                         &resolved) == TH_ENV_METHOD_FOUND
        && core_string_starts_with(resolved.qualified_name,
                                   CORE_STRING_STATIC("builtins."));
    ir_func_t *implementation = builtin ? nullptr
        : trait_implementation(methods, method, receiver->type, types);
    if (!implementation)
        return rewrite_primitive_comparison(function, call, method);
    ir_value_t *reference = ir_func_address(implementation->owner,
                                            implementation);
    if (!reference) return false;
    call->operands[0].value = reference;
    return true;
}

static void
specialize_trait_operators(ir_func_t *function, method_index_t *methods)
{
    core_list_t(selection_t) selections = core_list_new(selection_t);
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *call = function->blocks[b]->instrs[i];
            ir_instr_t *selection = call->op == IR_OP_CALL && call->n_operands
                && ir_operand_value(&call->operands[0])
                ? ir_operand_value(&call->operands[0])->def : nullptr;
            if (resolve_trait_operator(function, call, methods))
                core_list_push(selections, selection);
        }
    core_dict_t used;
    core_dict_init(&used, nullptr, nullptr);
    if (selections.len) ir_func_used_values(function, &used);
    for (size_t s = 0; s < selections.len; s++)
        if (selections.data[s]->block && selections.data[s]->dest
            && !core_dict_contains(&used, selections.data[s]->dest))
            (void)ir_instr_erase(selections.data[s]);
    core_dict_free(&used);
    core_list_free(selections);
}

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
    for (size_t i = 0; matched && i < count; i++) {
        const th_param *formal = th_func_param(target->type, i);
        ir_operand_t *operand = &call->operands[i + 1];
        matched = formal && operand->kind == IR_OPD_VALUE
            && operand->value && match_type(types, formal->type,
                call_operand_type(operand->value, types), &bindings);
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


bool n00bb_specialize_calls(ir_ctx_t *context,
    core_list_t(n00bb_ir_func_ptr_t) *functions,
    const n00bb_specialization_seed_t *seeds, size_t seed_count,
    core_list_t(n00bb_specialization_t) *instances,
    core_list_t(n00bb_specialization_request_t) *requests,
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
    method_index_t methods = {.functions = functions};
    core_dict_init_string(&methods.by_name);
    bool valid = true;
    for (size_t i = 0; i < instances->len; i++)
        add_specialization(&specializations, ((specialization_t){
            instances->data[i].generic, instances->data[i].instance,
            instances->data[i].signature, SIZE_MAX}));
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
            ((specialization_t){target, copy, actual, SIZE_MAX}));
        core_list_push(*instances,
            ((n00bb_specialization_t){target, copy, actual}));
        core_list_push(*functions, copy);
    }
    for (size_t index = 0; valid && index < functions->len; index++) {
        ir_func_t *caller = functions->data[index];
        size_t parent = instance_slot(&specializations, caller) - 1;
        th_ctx *caller_types = ir_module_types(caller->owner);
        bool open_caller = !th_is_closed(caller_types, caller->type)
            && parent == SIZE_MAX;
        if (!open_caller) specialize_trait_operators(caller, &methods);
        bool *feasible = fold_static_type_tests(caller, caller_types);
        valid = feasible != nullptr;
        for (uint32_t b = 0; valid && b < caller->n_blocks; b++) {
            if (!feasible[b]) continue;
            ir_block_t *block = caller->blocks[b];
            for (uint32_t i = 0; valid && i < block->n_instrs; i++) {
                ir_instr_t *call = block->instrs[i];
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
                        || integer_arguments_as(caller, call, target);
                    if (ir_func_is_always_inline(target)
                        && core_dict_add(&inline_bodies, target, 1))
                        core_list_push(*functions, target);
                    continue;
                }
                th_type *actual = open_caller || has_callback_operation(target)
                    ? nullptr : concrete_call_signature(call, target, types,
                                                        retargeted);
                core_list_t(type_binding_t) bindings = {0};
                bool concrete = actual && th_is_closed(types, actual)
                    && match_type(types, target->type, actual, &bindings)
                    && bindings.len;
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
                            ((specialization_t){target, copy, actual,
                                                parent}));
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
                if (valid) call->operands[0].value = bound;
            }
        }
        core_free(feasible);
    }
    specializations_free(&specializations);
    core_dict_free(&inline_bodies);
    method_index_free(&methods);
    return valid;
}
