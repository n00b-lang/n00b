#include "lower_internal.h"

static ir_func_t *
n00bb_record_equal_function(ir_lower_context_t *context, th_type *receiver);
static ir_func_t *
n00bb_record_hash_function(ir_lower_context_t *context, th_type *receiver);
static ir_func_t *
n00bb_scalar_hash_function(ir_lower_context_t *context, th_type *receiver);
static ir_func_t *
n00bb_object_equal_function(ir_lower_context_t *context, th_type *receiver);
static ir_func_t *
n00bb_object_hash_function(ir_lower_context_t *context, th_type *receiver);
static ir_func_t *
n00bb_record_finalize_function(ir_lower_context_t *context,
                               th_type *receiver);

bool
n00bb_emit_panic(ir_lower_context_t *context, ir_value_t *message,
                 ir_span_t span)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = state ? state->link_analysis : nullptr;
    for (size_t i = 0; analysis && i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        if (!core_string_eq(unit->module.identifier,
                            CORE_STRING_STATIC("builtin.panic"))) continue;
        n00bb_tu_state_t *owner = tu_state(unit);
        parse_scope_t *scope = module_scope(owner);
        parse_sym_entry_t *symbol = scope
            ? parse_symtab_lookup_in(scope, CORE_STRING_STATIC("panic"))
            : nullptr;
        ir_func_t *function = symbol
            ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
        ir_module_t *module = unit_module(owner);
        ir_value_t *callee = function && module
            ? ir_func_address(module, function) : nullptr;
        ir_instr_t *call = callee
            ? ir_instr_add(context->block, IR_OP_CALL) : nullptr;
        if (!call || !ir_instr_operand_value(call, callee)
            || !ir_instr_operand_value(call, message)
            || !ir_instr_set_call_signature(call, function->type))
            return false;
        call->span = span;
        return true;
    }
    return false;
}

static ir_value_t *
n00bb_equal_value(ir_block_t *block, ir_op_t op, core_string_t name,
                  th_type *type, ir_value_t *operand)
{
    ir_instr_t *in = ir_instr_add(block, op);
    return in && ir_instr_set_dest(in, name, type)
        && ir_instr_operand_value(in, operand) ? in->dest : nullptr;
}

static bool
n00bb_branch_on(ir_block_t *block, ir_value_t *test, ir_block_t *yes,
                ir_block_t *no)
{
    ir_instr_t *branch = test ? ir_instr_add(block, IR_OP_BRANCH) : nullptr;
    return branch && ir_instr_operand_value(branch, test)
        && ir_instr_operand_block(branch, yes)
        && ir_instr_operand_block(branch, no);
}

static bool
n00bb_equal_branch(ir_lower_context_t *context, n00bb_tu_state_t *state,
                   ir_block_t *current, const th_env_method_t *selected,
                   th_type *type, ir_value_t *left, ir_value_t *right,
                   ir_block_t *next, ir_block_t *mismatch, size_t field,
                   uint32_t depth)
{
    ir_func_t *function = current->owner;
    th_ctx *types = ir_module_types(context->module);
    th_type *boolean = th_name(types, CORE_STRING_STATIC("bool"));
    th_type *item = depth < 8 ? th_option_item(type) : nullptr;
    char buffer[6][64];
    if (item) {
        th_env_method_t inner = {0};
        if (th_env_method_resolve(state->types, item,
                CORE_STRING_STATIC("Equal"), CORE_STRING_STATIC("__equal"),
                &inner) != TH_ENV_METHOD_FOUND
            || (inner.error_type && !th_is_nil(inner.error_type)))
            return false;
        ir_value_t *left_nil = n00bb_equal_value(current, IR_OP_OPTION_IS_NIL,
            n00bb_format_name(buffer[0], sizeof buffer[0],
                              "field_%zu_%u_left_nil", field, depth),
            boolean, left);
        ir_value_t *right_nil = n00bb_equal_value(current,
            IR_OP_OPTION_IS_NIL,
            n00bb_format_name(buffer[1], sizeof buffer[1],
                              "field_%zu_%u_right_nil", field, depth),
            boolean, right);
        ir_block_t *absent = ir_block_add(function, n00bb_format_name(
            buffer[2], sizeof buffer[2], "field_%zu_%u_absent", field,
            depth));
        ir_block_t *present = ir_block_add(function, n00bb_format_name(
            buffer[3], sizeof buffer[3], "field_%zu_%u_present", field,
            depth));
        ir_block_t *both = ir_block_add(function, n00bb_format_name(
            buffer[4], sizeof buffer[4], "field_%zu_%u_both", field, depth));
        if (!left_nil || !right_nil || !absent || !present || !both
            || !n00bb_branch_on(current, left_nil, absent, present)
            || !n00bb_branch_on(absent, right_nil, next, mismatch)
            || !n00bb_branch_on(present, right_nil, mismatch, both))
            return false;
        ir_value_t *left_item = n00bb_equal_value(both, IR_OP_OPTION_GET,
            n00bb_format_name(buffer[0], sizeof buffer[0],
                              "field_%zu_%u_left_item", field, depth),
            item, left);
        ir_value_t *right_item = n00bb_equal_value(both, IR_OP_OPTION_GET,
            n00bb_format_name(buffer[1], sizeof buffer[1],
                              "field_%zu_%u_right_item", field, depth),
            item, right);
        return left_item && right_item
            && n00bb_equal_branch(context, state, both, &inner, item,
                                  left_item, right_item, next, mismatch,
                                  field, depth + 1);
    }
    core_string_t test_name = n00bb_format_name(buffer[5], sizeof buffer[5],
        "field_%zu_%u_equal", field, depth);
    ir_value_t *test = nullptr;
    if (selected->module.data
        && core_string_eq(selected->module, CORE_STRING_STATIC("builtins"))) {
        ir_instr_t *equal = ir_instr_add(current, IR_OP_EQ);
        if (equal && ir_instr_set_dest(equal, test_name, boolean)
            && ir_instr_operand_value(equal, left)
            && ir_instr_operand_value(equal, right)
            && (ir_machine_of(context->module, type).class != IR_CLASS_FLOAT
                || ir_instr_add_mod(equal, CORE_STRING_STATIC("ordered"))))
            test = equal->dest;
    }
    else test = n00bb_record_field_call(context, current, selected, left,
                                        right, boolean, test_name);
    return n00bb_branch_on(current, test, next, mismatch);
}

static ir_func_t *
n00bb_record_equal_function(ir_lower_context_t *context, th_type *receiver)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    th_type *shape = receiver && context
        ? ir_type_definition(context->module, receiver) : nullptr;
    if (!state || !types || !shape || th_kind_of(th_resolve(shape)) != TH_RECORD)
        return nullptr;
    ir_func_t *function = context->function;
    if (!function || function->n_blocks) return nullptr;
    th_type *boolean = th_name(types, CORE_STRING_STATIC("bool"));
    ir_value_t *left = ir_param_add(function, CORE_STRING_STATIC("left"), receiver);
    ir_value_t *right = ir_param_add(function, CORE_STRING_STATIC("right"), receiver);
    ir_block_t *current = ir_block_add(function, CORE_STRING_STATIC("entry"));
    ir_block_t *mismatch = ir_block_add(function, CORE_STRING_STATIC("mismatch"));
    ir_block_t *match = ir_block_add(function, CORE_STRING_STATIC("match"));
    if (!left || !right || !current || !mismatch || !match) return nullptr;
    for (size_t i = 0; i < th_record_len(shape); i++) {
        const th_member *field = th_record_member(shape, i);
        th_env_method_t selected = {0};
        if (!field || th_env_method_resolve(state->types, field->type,
                CORE_STRING_STATIC("Equal"), CORE_STRING_STATIC("__equal"),
                &selected) != TH_ENV_METHOD_FOUND
            || (selected.error_type
                && !th_is_nil(selected.error_type)))
            return nullptr;
        char left_buffer[64], right_buffer[64], next_buffer[64];
        core_string_t left_name = n00bb_format_name(left_buffer,
            sizeof left_buffer, "field_%zu_left", i);
        core_string_t right_name = n00bb_format_name(right_buffer,
            sizeof right_buffer, "field_%zu_right", i);
        core_string_t next_name = n00bb_format_name(next_buffer,
            sizeof next_buffer, "field_%zu_next", i);
        ir_instr_t *left_field = ir_instr_add(current, IR_OP_EXTRACT);
        ir_instr_t *right_field = ir_instr_add(current, IR_OP_EXTRACT);
        ir_block_t *next = ir_block_add(function, next_name);
        if (!left_field || !right_field || !next
            || !ir_instr_set_dest(left_field, left_name, field->type)
            || !ir_instr_operand_value(left_field, left)
            || !ir_instr_operand_name(left_field, field->name)
            || !ir_instr_set_dest(right_field, right_name, field->type)
            || !ir_instr_operand_value(right_field, right)
            || !ir_instr_operand_name(right_field, field->name)
            || !n00bb_equal_branch(context, state, current, &selected,
                                   field->type, left_field->dest,
                                   right_field->dest, next, mismatch, i, 0))
            return nullptr;
        current = next;
    }
    ir_instr_t *success = ir_instr_add(current, IR_OP_JUMP);
    ir_instr_t *false_value = ir_instr_add(mismatch, IR_OP_CONST);
    ir_instr_t *false_return = ir_instr_add(mismatch, IR_OP_RETURN);
    ir_instr_t *true_value = ir_instr_add(match, IR_OP_CONST);
    ir_instr_t *true_return = ir_instr_add(match, IR_OP_RETURN);
    if (!success || !ir_instr_operand_block(success, match)) return nullptr;
    if (!false_value || !ir_instr_set_dest(false_value,
                                           CORE_STRING_STATIC("false"),
                                           boolean)
        || !ir_instr_operand_int(false_value, 0)) return nullptr;
    if (!false_return || !ir_instr_operand_value(false_return,
                                                false_value->dest)) return nullptr;
    if (!true_value || !ir_instr_set_dest(true_value,
                                          CORE_STRING_STATIC("true"),
                                          boolean)
        || !ir_instr_operand_int(true_value, 1)) return nullptr;
    if (!true_return || !ir_instr_operand_value(true_return,
                                               true_value->dest)) return nullptr;
    return function;
}

static ir_func_t *
n00bb_record_hash_function(ir_lower_context_t *context, th_type *receiver)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    th_type *shape = receiver && context
        ? ir_type_definition(context->module, receiver) : nullptr;
    if (!state || !types || !shape || th_kind_of(th_resolve(shape)) != TH_RECORD)
        return nullptr;
    ir_func_t *function = context->function;
    if (!function || function->n_blocks) return nullptr;
    th_type *hash_type = th_name(types, CORE_STRING_STATIC("u128"));
    ir_value_t *self = ir_param_add(function, CORE_STRING_STATIC("self"), receiver);
    ir_block_t *block = ir_block_add(function, CORE_STRING_STATIC("entry"));
    ir_instr_t *zero = block ? ir_instr_add(block, IR_OP_CONST) : nullptr;
    if (!self || !zero || !ir_instr_set_dest(zero, CORE_STRING_STATIC("hash_zero"), hash_type)
        || !ir_instr_operand_int(zero, 0)) return nullptr;
    ir_value_t *accumulator = zero->dest;
    th_type *count_type = th_name(types, CORE_STRING_STATIC("u8"));
    ir_instr_t *one = ir_instr_add(block, IR_OP_CONST);
    ir_instr_t *last = ir_instr_add(block, IR_OP_CONST);
    if (!one || !last || !ir_instr_set_dest(one, CORE_STRING_STATIC("rotate_one"), count_type)
        || !ir_instr_operand_int(one, 1)
        || !ir_instr_set_dest(last, CORE_STRING_STATIC("rotate_last"), count_type)
        || !ir_instr_operand_int(last, 127)) return nullptr;
    for (size_t i = 0; i < th_record_len(shape); i++) {
        const th_member *field = th_record_member(shape, i);
        th_env_method_t selected = {0};
        if (!field || th_env_method_resolve(state->types, field->type,
                CORE_STRING_STATIC("builtin.hashing.Hash"), CORE_STRING_STATIC("hash"), &selected)
                != TH_ENV_METHOD_FOUND
            || (selected.error_type
                && !th_is_nil(selected.error_type)))
            return nullptr;
        char field_buffer[64], hash_buffer[64], left_buffer[64];
        char right_buffer[64], rotated_buffer[64], mixed_buffer[64];
        core_string_t field_name = n00bb_format_name(field_buffer,
            sizeof field_buffer, "field_%zu", i);
        core_string_t hash_name = n00bb_format_name(hash_buffer,
            sizeof hash_buffer, "field_%zu_hash", i);
        core_string_t left_name = n00bb_format_name(left_buffer,
            sizeof left_buffer, "hash_%zu_left", i);
        core_string_t right_name = n00bb_format_name(right_buffer,
            sizeof right_buffer, "hash_%zu_right", i);
        core_string_t rotated_name = n00bb_format_name(rotated_buffer,
            sizeof rotated_buffer, "hash_%zu_rotated", i);
        core_string_t mixed_name = n00bb_format_name(mixed_buffer,
            sizeof mixed_buffer, "hash_%zu_mixed", i);
        ir_instr_t *extract = ir_instr_add(block, IR_OP_EXTRACT);
        if (!extract || !ir_instr_set_dest(extract, field_name, field->type)
            || !ir_instr_operand_value(extract, self)
            || !ir_instr_operand_name(extract, field->name)) return nullptr;
        ir_value_t *field_hash = n00bb_record_field_call(context, block,
            &selected,
            extract->dest, nullptr, hash_type, hash_name);
        if (!field_hash) return nullptr;
        ir_instr_t *left = ir_instr_add(block, IR_OP_SHL);
        ir_instr_t *right = ir_instr_add(block, IR_OP_SHR);
        ir_instr_t *rotated = ir_instr_add(block, IR_OP_XOR);
        ir_instr_t *mixed = ir_instr_add(block, IR_OP_XOR);
        if (!left || !right || !rotated || !mixed
            || !ir_instr_set_dest(left, left_name, hash_type)
            || !ir_instr_operand_value(left, accumulator)
            || !ir_instr_operand_value(left, one->dest)
            || !ir_instr_set_dest(right, right_name, hash_type)
            || !ir_instr_operand_value(right, accumulator)
            || !ir_instr_operand_value(right, last->dest)
            || !ir_instr_set_dest(rotated, rotated_name, hash_type)
            || !ir_instr_operand_value(rotated, left->dest)
            || !ir_instr_operand_value(rotated, right->dest)
            || !ir_instr_set_dest(mixed, mixed_name, hash_type)
            || !ir_instr_operand_value(mixed, rotated->dest)
            || !ir_instr_operand_value(mixed, field_hash)) return nullptr;
        accumulator = mixed->dest;
    }
    ir_instr_t *ret = ir_instr_add(block, IR_OP_RETURN);
    return ret && ir_instr_operand_value(ret, accumulator)
        ? function : nullptr;
}

static ir_func_t *
n00bb_scalar_hash_function(ir_lower_context_t *context, th_type *receiver)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    ir_func_t *function = context ? context->function : nullptr;
    if (!lower || !lower->ir_context || !types || !function
        || function->n_blocks) return nullptr;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    th_type *hash_type = th_name(types, CORE_STRING_STATIC("u128"));
    bool flagset = n00bb_flagset_type(lower->unit, state, receiver, nullptr);
    parse_sym_entry_t *enum_symbol = flagset ? nullptr
        : n00bb_enum_type_symbol(state->link_analysis, receiver);
    const n00bb_enum_range_t *range = enum_symbol
        ? parse_sym_binding(enum_symbol,
            CORE_STRING_STATIC("n00bb.enum-range")) : nullptr;
    if (!flagset && !range) return nullptr;
    th_type *integer_type = th_name(types,
        range && range->storage_signed ? CORE_STRING_STATIC("i128") :CORE_STRING_STATIC("u128"));
    th_env_method_t selected = {0};
    if (th_env_method_resolve(state->types, integer_type,
            CORE_STRING_STATIC("builtin.hashing.Hash"), CORE_STRING_STATIC("hash"), &selected) != TH_ENV_METHOD_FOUND)
        return nullptr;
    ir_value_t *self = ir_param_add(function, CORE_STRING_STATIC("self"), receiver);
    ir_block_t *block = ir_block_add(function, CORE_STRING_STATIC("entry"));
    ir_instr_t *integer = block ? ir_instr_add(block,
        flagset ? IR_OP_BITSET_VALUE : IR_OP_ENUM_VALUE) : nullptr;
    if (!self || !integer
        || !ir_instr_set_dest(integer, CORE_STRING_STATIC("value"), integer_type)
        || !ir_instr_operand_value(integer, self)
        || (flagset && !ir_instr_add_mod(integer, CORE_STRING_STATIC("trap")))) return nullptr;
    ir_value_t *hashed = n00bb_record_field_call(context, block, &selected,
        integer->dest, nullptr, hash_type, CORE_STRING_STATIC("hash"));
    ir_instr_t *ret = hashed ? ir_instr_add(block, IR_OP_RETURN) : nullptr;
    return ret && ir_instr_operand_value(ret, hashed) ? function : nullptr;
}

// Enums and flagsets are equal when their values are.
static ir_func_t *
n00bb_scalar_equal_function(ir_lower_context_t *context, th_type *receiver)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    ir_func_t *function = context ? context->function : nullptr;
    if (!lower || !types || !function || function->n_blocks) return nullptr;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    bool flagset = n00bb_flagset_type(lower->unit, state, receiver, nullptr);
    parse_sym_entry_t *enum_symbol = flagset ? nullptr
        : n00bb_enum_type_symbol(state->link_analysis, receiver);
    const n00bb_enum_range_t *range = enum_symbol
        ? parse_sym_binding(enum_symbol,
            CORE_STRING_STATIC("n00bb.enum-range")) : nullptr;
    if (!flagset && !range) return nullptr;
    th_type *integer_type = th_name(types,
        range && range->storage_signed ? CORE_STRING_STATIC("i128")
                                       : CORE_STRING_STATIC("u128"));
    ir_value_t *self = ir_param_add(function, CORE_STRING_STATIC("self"),
                                    receiver);
    ir_value_t *other = ir_param_add(function, CORE_STRING_STATIC("other"),
                                     receiver);
    ir_block_t *block = ir_block_add(function, CORE_STRING_STATIC("entry"));
    ir_op_t op = flagset ? IR_OP_BITSET_VALUE : IR_OP_ENUM_VALUE;
    ir_instr_t *left = block ? ir_instr_add(block, op) : nullptr;
    ir_instr_t *right = block ? ir_instr_add(block, op) : nullptr;
    ir_instr_t *same = block ? ir_instr_add(block, IR_OP_EQ) : nullptr;
    if (!self || !other || !left || !right || !same
        || !ir_instr_set_dest(left, CORE_STRING_STATIC("left"), integer_type)
        || !ir_instr_operand_value(left, self)
        || (flagset && !ir_instr_add_mod(left, CORE_STRING_STATIC("trap")))
        || !ir_instr_set_dest(right, CORE_STRING_STATIC("right"),
                              integer_type)
        || !ir_instr_operand_value(right, other)
        || (flagset && !ir_instr_add_mod(right, CORE_STRING_STATIC("trap")))
        || !ir_instr_set_dest(same, CORE_STRING_STATIC("same"),
                              th_name(types, CORE_STRING_STATIC("bool")))
        || !ir_instr_operand_value(same, left->dest)
        || !ir_instr_operand_value(same, right->dest)) return nullptr;
    ir_instr_t *ret = ir_instr_add(block, IR_OP_RETURN);
    return ret && ir_instr_operand_value(ret, same->dest) ? function : nullptr;
}

static ir_func_t *
n00bb_object_equal_function(ir_lower_context_t *context, th_type *receiver)
{
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    ir_func_t *function = context ? context->function : nullptr;
    if (!types || !function || function->n_blocks) return nullptr;
    th_type *reference = n00bb_reference_definition(types, receiver);
    if (!reference) return nullptr;
    ir_value_t *self = ir_param_add(function, CORE_STRING_STATIC("self"),
                                    receiver);
    ir_value_t *other = ir_param_add(function, CORE_STRING_STATIC("other"),
                                     receiver);
    ir_block_t *block = ir_block_add(function, CORE_STRING_STATIC("entry"));
    ir_instr_t *left = block ? ir_instr_add(block, IR_OP_NOMINAL_CAST)
                             : nullptr;
    ir_instr_t *right = block ? ir_instr_add(block, IR_OP_NOMINAL_CAST)
                              : nullptr;
    ir_instr_t *same = block ? ir_instr_add(block, IR_OP_EQ) : nullptr;
    if (!self || !other || !left || !right || !same
        || !ir_instr_set_dest(left, CORE_STRING_STATIC("left"), reference)
        || !ir_instr_operand_value(left, self)
        || !ir_instr_set_dest(right, CORE_STRING_STATIC("right"), reference)
        || !ir_instr_operand_value(right, other)
        || !ir_instr_set_dest(same, CORE_STRING_STATIC("same"),
                              th_name(types, CORE_STRING_STATIC("bool")))
        || !ir_instr_operand_value(same, left->dest)
        || !ir_instr_operand_value(same, right->dest)) return nullptr;
    ir_instr_t *ret = ir_instr_add(block, IR_OP_RETURN);
    return ret && ir_instr_operand_value(ret, same->dest) ? function : nullptr;
}

static ir_func_t *
n00bb_object_hash_function(ir_lower_context_t *context, th_type *receiver)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    th_ctx *types = context ? ir_module_types(context->module) : nullptr;
    ir_func_t *function = context ? context->function : nullptr;
    if (!lower || !types || !function || function->n_blocks) return nullptr;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    th_type *reference = n00bb_reference_definition(types, receiver);
    th_env_method_t selected = {0};
    if (!reference
        || th_env_method_resolve(state->types, reference,
               CORE_STRING_STATIC("builtin.hashing.Hash"),
               CORE_STRING_STATIC("hash"), &selected)
            != TH_ENV_METHOD_FOUND) return nullptr;
    ir_value_t *self = ir_param_add(function, CORE_STRING_STATIC("self"),
                                    receiver);
    ir_block_t *block = ir_block_add(function, CORE_STRING_STATIC("entry"));
    ir_instr_t *view = block ? ir_instr_add(block, IR_OP_NOMINAL_CAST)
                             : nullptr;
    if (!self || !view
        || !ir_instr_set_dest(view, CORE_STRING_STATIC("address"), reference)
        || !ir_instr_operand_value(view, self)) return nullptr;
    ir_value_t *hashed = n00bb_record_field_call(context, block, &selected,
        view->dest, nullptr, th_name(types, CORE_STRING_STATIC("u128")),
        CORE_STRING_STATIC("hash"));
    ir_instr_t *ret = hashed ? ir_instr_add(block, IR_OP_RETURN) : nullptr;
    return ret && ir_instr_operand_value(ret, hashed) ? function : nullptr;
}

static ir_func_t *
n00bb_record_finalize_function(ir_lower_context_t *context,
                               th_type *receiver)
{
    ir_func_t *function = context->function;
    if (!function || function->n_blocks || !receiver) return nullptr;
    th_ctx *types = ir_module_types(context->module);
    ir_value_t *self = ir_param_add(function, CORE_STRING_STATIC("self"),
                                    th_ind(types, 0, receiver));
    ir_block_t *block = self
        ? ir_block_add(function, CORE_STRING_STATIC("entry")) : nullptr;
    if (!block) return nullptr;
    context->block = block;
    context->reachable = true;
    ir_span_t span = ir_module_span(context->module);
    span.implied = true;
    if (!n00bb_emit_finalize_fields(context, self, span, 0))
        return nullptr;
    ir_instr_t *ret = ir_instr_add(context->block, IR_OP_RETURN);
    return ret ? function : nullptr;
}

bool
n00bb_generate_value_methods(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (state->interface_module) continue;
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                th_type *receiver = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.generated-value-method"));
                if (!receiver || parse_sym_binding(symbol,
                        CORE_STRING_STATIC("n00bb.dispatch-adapter")))
                    continue;
                bool record_hash = symbol->name.data
                    && core_string_starts_with(symbol->name,
                        CORE_STRING_STATIC("__n00bb_record_hash_"));
                bool scalar_hash = symbol->name.data
                    && core_string_starts_with(symbol->name,
                        CORE_STRING_STATIC("__n00bb_scalar_hash_"));
                bool object_hash = symbol->name.data
                    && core_string_starts_with(symbol->name,
                        CORE_STRING_STATIC("__n00bb_object_hash_"));
                bool object_equal = symbol->name.data
                    && core_string_starts_with(symbol->name,
                        CORE_STRING_STATIC("__n00bb_object_equal_"));
                bool scalar_equal = symbol->name.data
                    && core_string_starts_with(symbol->name,
                        CORE_STRING_STATIC("__n00bb_scalar_equal_"));
                bool finalize = symbol->name.data
                    && core_string_starts_with(symbol->name,
                        CORE_STRING_STATIC("__finalize#n00bb_"));
                ir_func_t *function = link_ir_function_for_symbol(analysis, symbol);
                ir_module_t *module = unit_module(state);
                n00bb_lower_context_t lower = {
                    .unit = unit,
                    .ir_context = analysis->ir_context,
                };
                ir_lower_context_t context = {
                    .module = module,
                    .function = function,
                    .user = &lower,
                };
                bool generated = function && module
                    && (finalize
                        ? n00bb_record_finalize_function(&context, receiver)
                        : object_hash
                        ? n00bb_object_hash_function(&context, receiver)
                        : object_equal
                        ? n00bb_object_equal_function(&context, receiver)
                        : scalar_hash
                        ? n00bb_scalar_hash_function(&context, receiver)
                        : scalar_equal
                        ? n00bb_scalar_equal_function(&context, receiver)
                        : record_hash
                        ? n00bb_record_hash_function(&context, receiver)
                        : n00bb_record_equal_function(&context, receiver))
                    && ir_annot_set(analysis->ir_context, &function->annots,
                        (ir_annot_t){
                            .key = CORE_STRING_STATIC(
                                "n00bb.generated-value-method"),
                            .kind = IR_ANNOT_BOOL, .b = true});
                if (!generated) {
                    n00bb_internal_error(
                        core_translation_unit_diagnostics(unit), unit,
                        link_source_location(symbol->decl_node),
                        "generating value methods",
                        "could not generate value method `%.*s`",
                        (int)symbol->name.u8_bytes, symbol->name.data);
                    core_list_free(scopes);
                    return false;
                }
            }
        }
        core_list_free(scopes);
    }
    return true;
}

typedef struct {
    ir_lower_context_t *context;
    ir_func_t *target;
    th_type *signature;
    ir_value_t **inputs;
    uint32_t *optional;
    size_t optional_count;
} dispatch_adapter_t;

static ir_value_t *
n00bb_adapter_boxed(ir_lower_context_t *context, ir_value_t *value,
                    th_type *wanted)
{
    if (!value || th_kind_of(th_resolve(wanted)) != TH_VAR) return value;
    ir_instr_t *box = ir_lower_emit_instruction(context, IR_OP_BOX,
                                                IR_SPAN_UNKNOWN);
    return box && ir_lower_set_temp_dest(context, box,
            CORE_STRING_STATIC("__n00bb_adapter_boxed_"), wanted)
        && ir_instr_operand_value(box, value) ? box->dest : nullptr;
}

static bool
n00bb_adapter_tail(dispatch_adapter_t *adapter, uint64_t missing)
{
    ir_lower_context_t *context = adapter->context;
    ir_module_t *module = context->module;
    th_type *callee = th_resolve(adapter->target->type);
    th_type *success = th_func_ret(callee);
    th_type *error = th_func_error(callee);
    bool fails = error && th_is_closed(ir_module_types(module), error)
        && ir_machine_of(module, error).class != IR_CLASS_VOID;
    ir_instr_t *call = ir_lower_emit_instruction(context, IR_OP_CALL,
                                                 IR_SPAN_UNKNOWN);
    bool ok = call && success && ir_lower_set_temp_dest(context, call,
            CORE_STRING_STATIC("__n00bb_adapter_call_"),
            fails ? ir_call_outcome_type(module, callee) : success)
        && ir_instr_operand_value(call,
               ir_func_address(module, adapter->target));
    size_t optional_index = 0;
    for (size_t i = 0; ok && i < th_func_nparams(callee); i++) {
        const th_param *parameter = th_func_param(callee, i);
        bool optional = ir_func_parameter_is_optional(adapter->target,
                                                      (uint32_t)i);
        if (parameter->mode == TH_OUT) ok = ir_instr_operand_out(call);
        else if (optional && (missing >> optional_index & 1u))
            ok = ir_instr_operand_missing(call);
        else ok = ir_instr_operand_value(call, adapter->inputs[i]);
        optional_index += optional;
    }
    ok = ok && ir_instr_set_call_signature(call, adapter->target->type);
    ir_value_t *outcome = ok ? call->dest : nullptr;
    ir_value_t *value = outcome;
    if (ok && fails) {
        ir_block_t *ok_block = ir_block_add(context->function,
            CORE_STRING_STATIC("__n00bb_adapter_ok"));
        ir_block_t *error_block = ir_block_add(context->function,
            CORE_STRING_STATIC("__n00bb_adapter_error"));
        ok = ok_block && error_block && n00bb_branch_on_outcome(context,
            outcome, ok_block, error_block, IR_SPAN_UNKNOWN);
        context->block = error_block;
        ir_value_t *payload = ok ? n00bb_emit_outcome_payload(context,
            outcome, CORE_STRING_STATIC("error"), error, IR_SPAN_UNKNOWN)
            : nullptr;
        ir_instr_t *fail = payload
            ? ir_instr_add(context->block, IR_OP_FAIL) : nullptr;
        ok = fail && ir_instr_operand_value(fail, payload);
        context->block = ok_block;
        value = ok ? n00bb_emit_outcome_payload(context, outcome,
            CORE_STRING_STATIC("ok"), success, IR_SPAN_UNKNOWN) : nullptr;
        ok = value != nullptr;
    }
    th_type *returned = th_func_ret(adapter->signature);
    value = ok ? n00bb_adapter_boxed(context, value, returned) : nullptr;
    ir_instr_t *ret = value ? ir_instr_add(context->block, IR_OP_RETURN)
                            : nullptr;
    ok = ret && (!ir_callable_returns_value(module, adapter->signature)
                 || ir_instr_operand_value(ret, value));
    uint64_t ordinal = 0;
    for (size_t i = 0; ok && i < th_func_nparams(callee); i++) {
        const th_param *parameter = th_func_param(callee, i);
        if (!th_mode_produces_output(parameter->mode)) continue;
        ir_instr_t *output = ir_instr_insert(context->block,
            IR_OP_CALL_OUTPUT, ret->index);
        ok = output && ir_lower_set_temp_dest(context, output,
                CORE_STRING_STATIC("__n00bb_adapter_output_"),
                parameter->type)
            && ir_instr_operand_value(output, outcome)
            && ir_instr_operand_uint(output, ordinal++);
        th_type *wanted = th_func_param(adapter->signature, i)->type;
        if (ok && th_kind_of(th_resolve(wanted)) == TH_VAR) {
            ir_instr_t *box = ir_instr_insert(context->block, IR_OP_BOX,
                                              ret->index);
            ok = box && ir_lower_set_temp_dest(context, box,
                    CORE_STRING_STATIC("__n00bb_adapter_boxed_"), wanted)
                && ir_instr_operand_value(box, output->dest);
            output = ok ? box : nullptr;
        }
        ok = ok && ir_instr_operand_value(ret, output->dest);
    }
    return ok;
}

static bool
n00bb_adapter_presence(dispatch_adapter_t *adapter, size_t level,
                       uint64_t missing)
{
    if (level == adapter->optional_count)
        return n00bb_adapter_tail(adapter, missing);
    ir_lower_context_t *context = adapter->context;
    ir_value_t *parameter = adapter->context->function
        ->params[adapter->optional[level]];
    ir_instr_t *present = ir_lower_emit_instruction(context,
        IR_OP_PARAM_PRESENT, IR_SPAN_UNKNOWN);
    ir_block_t *given = ir_block_add(context->function,
        CORE_STRING_STATIC("__n00bb_adapter_given"));
    ir_block_t *omitted = ir_block_add(context->function,
        CORE_STRING_STATIC("__n00bb_adapter_omitted"));
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    ir_instr_t *branch = present && given && omitted && bool_type
            && ir_lower_set_temp_dest(context, present,
                   CORE_STRING_STATIC("__n00bb_adapter_present_"), bool_type)
            && ir_instr_operand_value(present, parameter)
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!branch || !ir_instr_operand_value(branch, present->dest)
        || !ir_instr_operand_block(branch, given)
        || !ir_instr_operand_block(branch, omitted)) return false;
    context->block = given;
    if (!n00bb_adapter_presence(adapter, level + 1, missing)) return false;
    context->block = omitted;
    return n00bb_adapter_presence(adapter, level + 1,
                                  missing | UINT64_C(1) << level);
}

static bool
n00bb_dispatch_adapter_body(core_translation_unit_t *unit,
                            ir_ctx_t *ir_context, ir_func_t *adapter,
                            ir_func_t *target, th_type *receiver)
{
    if (!adapter || adapter->n_blocks || !target || !target->type)
        return false;
    th_type *signature = th_resolve(adapter->type);
    size_t count = th_func_nparams(signature);
    n00bb_lower_context_t lower = {.unit = unit, .ir_context = ir_context};
    ir_lower_context_t context = {
        .module = adapter->owner,
        .function = adapter,
        .block = ir_block_add(adapter, CORE_STRING_STATIC("entry")),
        .user = &lower,
        .reachable = true,
    };
    dispatch_adapter_t state = {
        .context = &context,
        .target = target,
        .signature = signature,
        .inputs = count ? core_alloc_array(ir_value_t *, count) : nullptr,
        .optional = count ? core_alloc_array(uint32_t, count) : nullptr,
    };
    bool ok = context.block && (!count || (state.inputs && state.optional));
    for (size_t i = 0; ok && i < count; i++) {
        const th_param *parameter = th_func_param(signature, i);
        char buffer[32];
        core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
                                               "arg%zu", i);
        ir_value_t *value = ir_param_add(adapter, name, parameter->type);
        ok = value != nullptr;
        if (ok && ir_func_parameter_is_optional(target, (uint32_t)i)) {
            ok = state.optional_count < 64
                && ir_func_set_parameter_optional(adapter, (uint32_t)i,
                                                  true);
            state.optional[state.optional_count++] = (uint32_t)i;
        }
        state.inputs[i] = value;
        if (!ok || parameter->mode == TH_OUT
            || th_kind_of(th_resolve(parameter->type)) != TH_VAR)
            continue;
        ir_instr_t *narrow = ir_lower_emit_instruction(&context,
            IR_OP_TYPE_NARROW, IR_SPAN_UNKNOWN);
        ok = narrow && ir_lower_set_temp_dest(&context, narrow,
                CORE_STRING_STATIC("__n00bb_adapter_self_"), receiver)
            && ir_instr_operand_value(narrow, value)
            && ir_instr_operand_type(narrow, receiver);
        if (ok) state.inputs[i] = narrow->dest;
    }
    ok = ok && n00bb_adapter_presence(&state, 0, 0);
    core_free(state.inputs);
    core_free(state.optional);
    return ok;
}

bool
n00bb_generate_dispatch_adapters(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        if (state->interface_module) continue;
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                parse_sym_entry_t *implementation = parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.dispatch-adapter"));
                th_type *receiver = implementation ? parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.generated-value-method"))
                    : nullptr;
                if (!receiver) continue;
                ir_func_t *adapter = link_ir_function_for_symbol(analysis,
                                                                 symbol);
                ir_func_t *target = link_ir_function_for_symbol(analysis,
                                                                implementation);
                if (target && target->n_blocks
                    && n00bb_dispatch_adapter_body(unit, analysis->ir_context,
                                                   adapter, target, receiver)
                    && ir_annot_set(analysis->ir_context, &adapter->annots,
                        (ir_annot_t){
                            .key = CORE_STRING_STATIC(
                                "n00bb.generated-value-method"),
                            .kind = IR_ANNOT_BOOL, .b = true}))
                    continue;
                n00bb_internal_error(core_translation_unit_diagnostics(unit),
                    unit, link_source_location(implementation->decl_node),
                    "generating dispatch adapters",
                    "could not generate the dispatch adapter for `%.*s`",
                    (int)implementation->name.u8_bytes,
                    implementation->name.data);
                core_list_free(scopes);
                return false;
            }
        }
        core_list_free(scopes);
    }
    return true;
}
