#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "specialize.h"
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
        const char *left = nullptr, *right = nullptr;
        size_t left_len = 0, right_len = 0, left_count = 0, right_count = 0;
        bool left_variant = false, right_variant = false;
        if (!th_named_info(pattern, &left, &left_len, &left_variant,
                &left_count)
            || !th_named_info(actual, &right, &right_len, &right_variant,
                &right_count)
            || left_len != right_len || memcmp(left, right, left_len)
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
                if (a->str.n != b->str.n
                    || memcmp(a->str.p, b->str.p, a->str.n)) return false;
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
            if (!a || !b || strcmp(a->name, b->name)
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
            if (!a || !b || strcmp(a->name, b->name)
                || !match_type(types, a->type, b->type, bindings))
                return false;
        }
        return true;
    default:
        return th_hash(types, pattern) == th_hash(types, actual);
    }
}

static bool has_polymorphic_cycle(
    const core_list_t(specialization_t) *specializations,
    size_t parent, ir_func_t *target, th_type *signature, th_ctx *types)
{
    for (size_t i = parent; i != SIZE_MAX;
         i = specializations->data[i].parent) {
        const specialization_t *item = &specializations->data[i];
        if (item->source == target
            && th_hash(types, item->signature) != th_hash(types, signature))
            return true;
    }
    return false;
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

static th_type *
concrete_call_signature(ir_instr_t *call, ir_func_t *target, th_ctx *types)
{
    th_type *signature = ir_instr_call_signature(call);
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
    const char *owner = ir_module_name(target->owner);
    core_buffer_t identity = {0};
    core_buffer_printf(&identity, "%s.%s:%016" PRIx64, owner, target->name,
                       th_hash(types, actual));
    core_buffer_t name = {0};
    if (identity.data)
        core_buffer_printf(&name, "%s$spec$%016" PRIx64, owner,
            core_xxh3_64bits_raw(identity.data, identity.byte_len));
    core_buffer_release(&identity);
    ir_module_t *module = name.data ? ir_module_new(context, name.data,
        ir_module_profile(target->owner), ir_module_target(target->owner))
        : nullptr;
    core_buffer_release(&name);
    const th_type **from = core_alloc_array(const th_type *, bindings->len);
    th_type **to = core_alloc_array(th_type *, bindings->len);
    const char *clone_failure = nullptr;
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
    if (!copy && failure)
        core_buffer_printf(failure,
            "could not clone %s.%s for concrete signature %s: %s",
            owner, target->name, th_print(types, actual), clone_failure
                ? clone_failure : "allocation failed");
    return copy;
}

static ir_func_t *
function_body(ir_func_t *function)
{
    if (!function || function->n_blocks) return function;
    ir_annot_t *body = ir_annot_get(&function->annots, "n00bb.cached_body");
    return body && body->kind == IR_ANNOT_VALUE && body->v
        && body->v->kind == IR_VAL_FUNC ? body->v->owner : function;
}

static ir_func_t *
find_instance(const core_list_t(specialization_t) *specializations,
              ir_func_t *target, uint64_t hash, th_ctx *types)
{
    for (size_t n = 0; n < specializations->len; n++)
        if (specializations->data[n].source == target
            && th_hash(types, specializations->data[n].signature) == hash)
            return specializations->data[n].copy;
    return nullptr;
}

static bool
is_instance(const core_list_t(specialization_t) *specializations,
            const ir_func_t *function)
{
    for (size_t n = 0; n < specializations->len; n++)
        if (specializations->data[n].copy == function) return true;
    return false;
}

bool n00bb_specialize_calls(ir_ctx_t *context,
    core_list_t(n00bb_ir_func_ptr_t) *functions,
    core_list_t(n00bb_specialization_t) *instances,
    core_list_t(n00bb_specialization_request_t) *requests,
    core_buffer_t *failure)
{
    if (!context || !functions || !instances || !requests) return false;
    core_list_t(specialization_t) specializations = {0};
    bool valid = true;
    size_t original_count = functions->len;
    for (size_t index = 0; valid && index < functions->len; index++) {
        ir_func_t *caller = functions->data[index];
        size_t parent = SIZE_MAX;
        if (index >= original_count)
            parent = index - original_count;
        if (!th_is_closed(ir_module_types(caller->owner), caller->type)
            && !is_instance(&specializations, caller)) continue;
        for (uint32_t b = 0; valid && b < caller->n_blocks; b++) {
            ir_block_t *block = caller->blocks[b];
            for (uint32_t i = 0; valid && i < block->n_instrs; i++) {
                ir_instr_t *call = block->instrs[i];
                if (call->op != IR_OP_CALL || !call->n_operands) continue;
                ir_value_t *callee = ir_operand_value(&call->operands[0]);
                ir_func_t *target = callee && callee->kind == IR_VAL_FUNC
                    ? function_body(callee->owner) : nullptr;
                if (!target || !target->n_blocks
                    || is_instance(&specializations, target)) continue;
                th_ctx *types = ir_module_types(target->owner);
                if (th_is_closed(types, target->type)) continue;
                th_type *actual = has_callback_operation(target) ? nullptr
                    : concrete_call_signature(call, target, types);
                core_list_t(type_binding_t) bindings = {0};
                bool concrete = actual && th_is_closed(types, actual)
                    && match_type(types, target->type, actual, &bindings)
                    && bindings.len && !has_polymorphic_cycle(
                        &specializations, parent, target, actual, types);
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
                        core_list_push(specializations,
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
                if (!valid && failure)
                    core_buffer_printf(failure,
                        "could not bind specialized function %s.%s",
                        ir_module_name(copy->owner), copy->name);
                if (valid) call->operands[0].value = bound;
            }
        }
    }
    core_list_free(specializations);
    return valid;
}
