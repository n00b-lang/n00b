#include "abi_type_index.h"

#include <limits.h>
#include <string.h>
#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/indexed_table.h"
#include "lib/list.h"
#include "signature_typevars.h"
#include "abi_decl_index.h"
#include "cached_body.h"

typedef struct {
    uint64_t id;
} type_key_t;

typedef type_key_t *type_key_ptr_t;
core_list_decl(type_key_ptr_t);

struct n00bb_abi_type_index_t {
    ir_module_t *module;
    core_indexed_table_t *table;
    core_dict_t symbol_types;
    core_list_t(type_key_ptr_t) keys;
    bool failed;
};

static core_hash_value_t
type_key_hash(void *opaque)
{
    return ((const type_key_t *)opaque)->id;
}

static bool
type_key_equal(const void *left, const void *right)
{
    return ((const type_key_t *)left)->id == ((const type_key_t *)right)->id;
}

static bool add_type(n00bb_abi_type_index_t *index, th_type *type,
                     uint32_t *out_index);
static bool add_children(n00bb_abi_type_index_t *index, th_type *type);
static bool collect_call_specializations(n00bb_abi_type_index_t *index,
                                         ir_module_t *module,
                                         ir_func_t *const *functions,
                                         size_t function_count);

static bool
add_optional(n00bb_abi_type_index_t *index, th_type *type)
{
    return !type || add_type(index, type, nullptr);
}

static bool
add_initializer_types(n00bb_abi_type_index_t *index,
                      const ir_global_init_value_t *value, size_t depth)
{
    if (!value) return true;
    if (depth > 64) return false;
    if (value->kind == IR_GLOBAL_INIT_TYPE)
        return value->descriptor_type
            && add_type(index, value->descriptor_type, nullptr);
    if (value->kind != IR_GLOBAL_INIT_SEQUENCE
        && value->kind != IR_GLOBAL_INIT_TUPLE
        && value->kind != IR_GLOBAL_INIT_RECORD
        && value->kind != IR_GLOBAL_INIT_VARIANT
        && value->kind != IR_GLOBAL_INIT_SET
        && value->kind != IR_GLOBAL_INIT_DICTIONARY) return true;
    if (value->aggregate.count && !value->aggregate.items) return false;
    for (size_t i = 0; i < value->aggregate.count; i++)
        if (!add_initializer_types(index, &value->aggregate.items[i],
                                   depth + 1)) return false;
    return true;
}

static bool
add_constraint(n00bb_abi_type_index_t *index, th_constraint constraint)
{
    switch (constraint.kind) {
    case TH_CON_UNIFIES:
    case TH_CON_PROMOTES:
    case TH_CON_EXCLUDES:
        return add_optional(index, constraint.target);
    case TH_CON_ONE_OF:
        for (size_t i = 0; i < constraint.one_of.n; i++)
            if (!add_optional(index, constraint.one_of.types[i])) return false;
        return true;
    case TH_CON_HAS_FIELD:
        return add_optional(index, constraint.field.type);
    case TH_CON_HAS_PARAM:
        return add_optional(index, constraint.param.type);
    case TH_CON_METHOD:
        return add_optional(index, constraint.method.result)
            && (!constraint.method.call
                || (th_kind_of(th_resolve(constraint.method.call))
                        == TH_FUNCTION
                    && add_children(index, th_resolve(constraint.method.call))));
    case TH_CON_IMPLEMENTS:
    case TH_CON_KIND:
    case TH_CON_INTEGER:
        return true;
    }
    return false;
}

static bool
add_reference_to_closed_child(n00bb_abi_type_index_t *index, th_type *child)
{
    if (!child || !th_is_closed(ir_module_types(index->module), child))
        return true;
    th_type *reference = th_ind(ir_module_types(index->module), 0, child);
    return reference && add_type(index, reference, nullptr);
}

static bool
add_children(n00bb_abi_type_index_t *index, th_type *type)
{
    switch (th_kind_of(type)) {
    case TH_NAMED:
        for (size_t i = 0; i < th_named_nargs(type); i++) {
            const th_arg *arg = th_named_arg(type, i);
            if (!arg || (arg->kind == TH_ARG_TYPE
                         && !add_optional(index, arg->type))) return false;
        }
        th_type *definition = ir_type_definition(index->module, type);
        return definition && th_kind_of(definition) == TH_RECORD
            ? add_children(index, definition)
            : add_optional(index, definition);
    case TH_BITINT:
    case TH_BOUND:
        return true;
    case TH_INDIRECTION:
        return add_optional(index, th_ind_referent(type));
    case TH_SEQUENCE:
        return add_optional(index, th_seq_elem(type))
            && add_reference_to_closed_child(index, th_seq_elem(type));
    case TH_RECORD:
        for (size_t i = 0; i < th_record_len(type); i++) {
            const th_member *member = th_record_member(type, i);
            if (!member || !add_optional(index, member->type)
                || !add_reference_to_closed_child(index, member->type))
                return false;
        }
        return true;
    case TH_FUNCTION:
        if (!add_optional(index, th_func_ret(type))
            || !add_optional(index, th_func_error(type))) return false;
        for (size_t i = 0; i < th_func_nparams(type); i++) {
            const th_param *param = th_func_param(type, i);
            if (!param || !add_optional(index, param->type)) return false;
        }
        const th_param *rest = th_func_rest(type);
        if (rest && !add_optional(index, rest->type)) return false;
        for (size_t i = 0; i < th_func_nkw(type); i++) {
            const th_kwparam *param = th_func_kw(type, i);
            if (!param || !add_optional(index, param->type)) return false;
        }
        return true;
    case TH_BINDER:
        return add_optional(index, th_binder_body(type));
    case TH_SUM:
        for (size_t i = 0; i < th_sum_len(type); i++)
            if (!add_optional(index, th_sum_arm(type, i))) return false;
        return true;
    case TH_VAR:
        for (size_t i = 0; i < th_var_constraint_count(type); i++) {
            th_constraint constraint;
            if (!th_var_constraint_at(type, i, &constraint)
                || !add_constraint(index, constraint)) return false;
        }
        return true;
    case TH_TRAIT:
        for (size_t i = 0; i < th_trait_len(type); i++) {
            const th_member *method = th_trait_method(type, i);
            if (!method || !add_optional(index, method->type)) return false;
        }
        for (size_t i = 0; i < th_trait_requirement_count(type); i++) {
            th_trait_requirement requirement;
            if (!th_trait_requirement_at(type, i, &requirement)
                || !add_optional(index, requirement.subject)
                || !add_optional(index, requirement.target)) return false;
        }
        return true;
    }
    return false;
}

static bool
add_type_with_children(n00bb_abi_type_index_t *index, th_type *type,
                       uint32_t *out_index, bool collect_children)
{
    if (!index || index->failed || !type) return false;
    type = th_resolve(type);
    type_key_t probe = {.id = ir_type_id(index->module, type)};
    const core_indexed_entry_t *entry = core_indexed_table_find(
        index->table, &probe);
    if (entry) {
        if (out_index) *out_index = entry->index;
        return true;
    }
    type_key_t *key = core_alloc(type_key_t);
    *key = probe;
    entry = core_indexed_table_intern(index->table, key, type, nullptr);
    if (!entry) {
        core_free(key);
        index->failed = true;
        return false;
    }
    core_list_push(index->keys, key);
    if (out_index) *out_index = entry->index;
    if (collect_children && !add_children(index, type)) {
        index->failed = true;
        return false;
    }
    return true;
}

static bool
add_type(n00bb_abi_type_index_t *index, th_type *type,
         uint32_t *out_index)
{
    return add_type_with_children(index, type, out_index, true);
}

static bool
add_open_type_parts(n00bb_abi_type_index_t *index, th_type *type,
                    unsigned depth)
{
    th_ctx *types = ir_module_types(index->module);
    type = type ? th_resolve(type) : nullptr;
    if (!type || depth > 64) return !type;
    if (th_is_closed(types, type)) return add_type(index, type, nullptr);
    switch (th_kind_of(type)) {
    case TH_NAMED: {
        th_type *head = n00bb_generic_head(types, type);
        if (head && !add_type(index, head, nullptr)) return false;
        for (size_t i = 0; i < th_named_nargs(type); i++) {
            const th_arg *argument = th_named_arg(type, i);
            if (argument && argument->kind == TH_ARG_TYPE
                && !add_open_type_parts(index, argument->type, depth + 1))
                return false;
        }
        return true;
    }
    case TH_SEQUENCE:
        return add_open_type_parts(index, th_seq_elem(type), depth + 1);
    case TH_INDIRECTION:
        return add_open_type_parts(index, th_ind_referent(type), depth + 1);
    default:
        return true;
    }
}

n00bb_abi_type_index_t *
n00bb_abi_type_index_new(ir_module_t *module)
{
    if (!module || !ir_module_types(module)) return nullptr;
    n00bb_abi_type_index_t *index = core_alloc(n00bb_abi_type_index_t);
    index->module = module;
    index->table = core_indexed_table_new(1, UINT32_MAX >> 1,
                                          type_key_hash, type_key_equal);
    core_dict_init(&index->symbol_types, nullptr, nullptr);
    th_type *nil = th_name(ir_module_types(module), CORE_STRING_STATIC("nil"));
    if (!index->table || !nil) {
        n00bb_abi_type_index_free(index);
        return nullptr;
    }
    type_key_t *key = core_alloc(type_key_t);
    *key = (type_key_t){.id = ir_type_id(module, nil)};
    if (!core_indexed_table_reserve(index->table, 0, key, nil)) {
        core_free(key);
        n00bb_abi_type_index_free(index);
        return nullptr;
    }
    core_list_push(index->keys, key);
    return index;
}

void
n00bb_abi_type_index_free(n00bb_abi_type_index_t *index)
{
    if (!index) return;
    core_dict_free(&index->symbol_types);
    core_indexed_table_free(index->table);
    for (size_t i = 0; i < index->keys.len; i++)
        core_free(index->keys.data[i]);
    core_list_free(index->keys);
    core_free(index);
}

bool
n00bb_abi_type_index_add(n00bb_abi_type_index_t *index,
                          th_type *type, uint32_t *out_index)
{
    return add_type(index, type, out_index);
}

bool
n00bb_abi_type_index_add_imported_trait(n00bb_abi_type_index_t *index,
                                         th_type *type)
{
    if (!type || th_kind_of(th_resolve(type)) != TH_TRAIT) return false;
    return add_type_with_children(index, type, nullptr, false);
}

size_t
n00bb_abi_type_index_count(const n00bb_abi_type_index_t *index)
{
    return index ? core_indexed_table_len(index->table) : 0;
}

th_type *
n00bb_abi_type_index_at(const n00bb_abi_type_index_t *index, uint32_t row)
{
    const core_indexed_entry_t *entry = index
        ? core_indexed_table_at(index->table, row) : nullptr;
    return entry ? entry->value : nullptr;
}

bool
n00bb_abi_type_index_find(n00bb_abi_type_index_t *index,
                           th_type *type, uint32_t *out_index)
{
    if (!index || !type || !out_index) return false;
    type_key_t probe = {.id = ir_type_id(index->module, th_resolve(type))};
    const core_indexed_entry_t *entry = core_indexed_table_find(
        index->table, &probe);
    if (!entry) return false;
    *out_index = entry->index;
    return true;
}

static bool
add_string_backing(n00bb_abi_type_index_t *index)
{
    th_ctx *types = ir_module_types(index->module);
    uint32_t found = 0;
    return !n00bb_abi_type_index_find(index,
            th_name(types, CORE_STRING_STATIC("string")), &found)
        || add_type(index, th_name(types, CORE_STRING_STATIC("u8")), nullptr);
}

bool
n00bb_abi_type_index_collect_module_types(n00bb_abi_type_index_t *index,
                                          ir_module_t *module)
{
    if (!index || !module || index->failed
        || ir_module_types(module) != ir_module_types(index->module))
        return false;
    th_ctx *types = ir_module_types(module);
    for (size_t i = 0; i < ir_type_count(module); i++) {
        th_type *type = ir_type_at(module, i);
        if (!type) return false;
        th_type *resolved = th_resolve(type);
        /* Inference-only call shapes remain interned; symbol closure still
         * collects and rejects any incomplete shape that is reachable. */
        if (th_kind_of(resolved) == TH_FUNCTION
            && !th_func_error(resolved)) continue;
        if (th_is_closed(types, type)
            && !add_type(index, type, nullptr)) return false;
        if (!th_is_closed(types, type)
            && !add_open_type_parts(index, resolved, 0)) return false;
    }
    for (size_t i = 0; i < ir_global_count(module); i++) {
        ir_global_t *global = ir_global_at(module, i);
        if (!global || !add_initializer_types(index,
            global->static_initializer, 0)) return false;
    }
    return collect_call_specializations(index, module, nullptr, 0)
        && add_string_backing(index);
}

static bool
add_closed_type(n00bb_abi_type_index_t *index, th_type *type)
{
    return !type || !th_is_closed(ir_module_types(index->module), type)
        || add_type(index, type, nullptr);
}

typedef struct {
    const th_type *variable;
    th_type *actual;
} call_type_binding_t;
core_list_decl(call_type_binding_t);

typedef ir_func_t *inline_body_t;
core_list_decl(inline_body_t);

typedef struct {
    ir_func_t *target;
    th_type *actual;
    uint64_t hash;
} call_specialization_t;
core_list_decl(call_specialization_t);

typedef struct {
    core_list_t(call_specialization_t) list;
    core_dict_t                        hashes_of;
} pending_calls_t;

static void
pending_add(pending_calls_t *pending, ir_func_t *target, th_type *actual,
            uint64_t hash)
{
    core_dict_t *hashes = core_dict_get(&pending->hashes_of, target, nullptr);
    if (!hashes) {
        hashes = core_alloc(core_dict_t);
        core_dict_init(hashes, nullptr, nullptr);
        core_dict_put(&pending->hashes_of, target, hashes);
    }
    if (core_dict_add(hashes, hash, 1))
        core_list_push(pending->list,
                       ((call_specialization_t){target, actual, hash}));
}

static void
pending_free(pending_calls_t *pending)
{
    core_dict_foreach(&pending->hashes_of, item) {
        core_dict_free(item->value);
        core_free(item->value);
    }
    core_dict_free(&pending->hashes_of);
    core_list_free(pending->list);
}

static bool
match_call_type(th_ctx *types, th_type *pattern, th_type *actual,
                core_list_t(call_type_binding_t) *bindings, unsigned depth)
{
    if (!pattern || !actual || depth > 64) return false;
    pattern = th_resolve(pattern);
    actual = th_resolve(actual);
    th_kind kind = th_kind_of(pattern);
    if (kind == TH_VAR) {
        for (size_t i = 0; i < bindings->len; i++)
            if (bindings->data[i].variable == pattern)
                return th_hash(types, bindings->data[i].actual)
                    == th_hash(types, actual);
        core_list_push(*bindings, ((call_type_binding_t){pattern, actual}));
        return true;
    }
    if (kind != th_kind_of(actual)) return true;
    if (kind == TH_SEQUENCE)
        return match_call_type(types, th_seq_elem(pattern),
            th_seq_elem(actual), bindings, depth + 1);
    if (kind == TH_INDIRECTION)
        return match_call_type(types, th_ind_referent(pattern),
            th_ind_referent(actual), bindings, depth + 1);
    if (kind == TH_NAMED) {
        core_string_t left = {}, right = {};
        size_t left_count = 0, right_count = 0;
        if (!th_named_info(pattern, &left, nullptr, &left_count)
            || !th_named_info(actual, &right, nullptr, &right_count)
            || !left.data || !right.data || !core_string_eq(left, right)
            || left_count != right_count) return false;
        for (size_t i = 0; i < left_count; i++) {
            const th_arg *a = th_named_arg(pattern, i);
            const th_arg *b = th_named_arg(actual, i);
            if (!a || !b || a->kind != b->kind) return false;
            if (a->kind == TH_ARG_TYPE
                && !match_call_type(types, a->type, b->type,
                    bindings, depth + 1)) return false;
        }
        return true;
    }
    if (kind == TH_FUNCTION) {
        if (th_func_nparams(pattern) != th_func_nparams(actual)
            || !match_call_type(types, th_func_ret(pattern),
                th_func_ret(actual), bindings, depth + 1)) return false;
        th_type *left_error = th_func_error(pattern);
        th_type *right_error = th_func_error(actual);
        if (!!left_error != !!right_error
            || (left_error && !match_call_type(types, left_error,
                right_error, bindings, depth + 1))) return false;
        for (size_t i = 0; i < th_func_nparams(pattern); i++) {
            const th_param *a = th_func_param(pattern, i);
            const th_param *b = th_func_param(actual, i);
            if (!a || !b || !match_call_type(types, a->type, b->type,
                    bindings, depth + 1)) return false;
        }
    }
    return true;
}

static bool
collect_specialized_type(n00bb_abi_type_index_t *index, th_type *type,
                         const th_type **from, th_type **to, size_t count)
{
    if (!type || th_is_closed(ir_module_types(index->module), type))
        return add_closed_type(index, type);
    th_type *closed = th_type_clone_replacing(ir_module_types(index->module),
        type, from, to, count);
    return closed && add_closed_type(index, closed);
}

static bool
append_call_specialization(n00bb_abi_type_index_t *index,
                           pending_calls_t *pending,
                           ir_instr_t *instruction, const th_type **from,
                           th_type **to, size_t count)
{
    if (instruction->op != IR_OP_CALL || !instruction->n_operands)
        return true;
    ir_value_t *callee = ir_operand_value(&instruction->operands[0]);
    ir_func_t *target = callee && callee->kind == IR_VAL_FUNC
        ? callee->owner : nullptr;
    th_type *actual = ir_instr_call_signature(instruction);
    th_ctx *types = ir_module_types(index->module);
    if (!target || !target->type || !actual
        || th_is_closed(types, target->type)) return true;
    if (count && !th_is_closed(types, actual))
        actual = th_type_clone_replacing(types, actual, from, to, count);
    if (!actual) return false;
    if (!th_is_closed(types, actual)) return true;
    pending_add(pending, target, actual, th_hash(types, actual));
    return true;
}

static bool
collect_specialized_body(n00bb_abi_type_index_t *index, ir_func_t *function,
                         const th_type **from, th_type **to, size_t count,
                         pending_calls_t *pending)
{
    for (uint32_t i = 0; i < function->n_locals; i++)
        if (!collect_specialized_type(index, function->locals[i]->type,
                from, to, count)) return false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (!append_call_specialization(index, pending, instruction,
                    from, to, count)) return false;
            if (instruction->dest
                && !collect_specialized_type(index, instruction->dest->type,
                    from, to, count)) return false;
            for (uint32_t o = 0; o < instruction->n_operands; o++) {
                ir_operand_t *operand = &instruction->operands[o];
                ir_value_t *value = ir_operand_value(operand);
                if ((value && !collect_specialized_type(index, value->type,
                        from, to, count))
                    || (operand->kind == IR_OPD_TYPE
                        && !collect_specialized_type(index, operand->type,
                            from, to, count))) return false;
            }
        }
    }
    return true;
}

static void
append_inline_body(core_list_t(inline_body_t) *bodies, core_dict_t *seen,
                   const ir_instr_t *instruction, const ir_module_t *module)
{
    ir_func_t *body = n00bb_direct_call_target(instruction);
    if (!body || !body->n_blocks || body->owner == module
        || !ir_func_is_always_inline(body) || !core_dict_add(seen, body, body))
        return;
    core_list_push(*bodies, body);
}

static bool
collect_call_specializations(n00bb_abi_type_index_t *index,
                             ir_module_t *module,
                             ir_func_t *const *functions,
                             size_t function_count)
{
    th_ctx *types = ir_module_types(index->module);
    pending_calls_t pending = {0};
    core_dict_init(&pending.hashes_of, nullptr, nullptr);
    bool valid = true;
    size_t count = module ? ir_func_count(module) : function_count;
    core_list_t(inline_body_t) inline_bodies = {0};
    core_dict_t inline_seen;
    core_dict_init(&inline_seen, nullptr, nullptr);
    for (size_t f = 0; f < count; f++) {
        ir_func_t *caller = module ? ir_func_at(module, f) : functions[f];
        for (uint32_t b = 0; b < caller->n_blocks; b++) {
            ir_block_t *block = caller->blocks[b];
            for (uint32_t i = 0; valid && i < block->n_instrs; i++) {
                valid = append_call_specialization(index, &pending,
                    block->instrs[i], nullptr, nullptr, 0);
                append_inline_body(&inline_bodies, &inline_seen,
                                   block->instrs[i], module);
            }
        }
    }
    for (size_t n = 0; valid && n < inline_bodies.len; n++) {
        ir_func_t *body = inline_bodies.data[n];
        valid = collect_specialized_body(index, body, nullptr, nullptr, 0,
                                         &pending);
        for (uint32_t b = 0; valid && b < body->n_blocks; b++)
            for (uint32_t i = 0; i < body->blocks[b]->n_instrs; i++)
                append_inline_body(&inline_bodies, &inline_seen,
                                   body->blocks[b]->instrs[i], module);
    }
    core_dict_free(&inline_seen);
    core_list_free(inline_bodies);
    for (size_t i = 0; valid && i < pending.list.len; i++) {
        call_specialization_t specialization = pending.list.data[i];
        core_list_t(call_type_binding_t) bindings = {0};
        valid = match_call_type(types, specialization.target->type,
            specialization.actual, &bindings, 0);
        const th_type **from = valid && bindings.len
            ? core_alloc_array(const th_type *, bindings.len) : nullptr;
        th_type **to = valid && bindings.len
            ? core_alloc_array(th_type *, bindings.len) : nullptr;
        if (valid && bindings.len && (!from || !to)) valid = false;
        for (size_t n = 0; valid && n < bindings.len; n++) {
            from[n] = bindings.data[n].variable;
            to[n] = bindings.data[n].actual;
        }
        if (valid && bindings.len)
            valid = collect_specialized_body(index,
                specialization.target, from, to, bindings.len, &pending);
        core_free(from);
        core_free(to);
        core_list_free(bindings);
    }
    pending_free(&pending);
    return valid;
}

static th_type *
declaration_type(n00bb_abi_type_index_t *index,
                 const parse_sym_entry_t *symbol, th_type *type)
{
    if (!type) return nullptr;
    for (parse_scope_t *scope = symbol->scope; scope; scope = scope->parent) {
        if (!core_string_eq(scope->scope_tag,
                            CORE_STRING_STATIC("function"))) continue;
        parse_sym_entry_t *function = scope->node
            ? parse_tree_sym(scope->node) : nullptr;
        n00bb_body_typevar_map_t *map = function
            ? parse_sym_binding(function, N00BB_BODY_TYPEVAR_MAP_KEY)
            : nullptr;
        if (!map || !map->count) return type;
        th_type **free_vars = nullptr;
        size_t free_count = th_free_vars(ir_module_types(index->module), type,
                                        &free_vars);
        bool needs_rebase = false;
        for (size_t i = 0; i < free_count && !needs_rebase; i++)
            for (size_t j = 0; j < map->count; j++)
                if (free_vars[i] == th_resolve(map->body[j])) {
                    needs_rebase = true;
                    break;
                }
        if (!needs_rebase) return type;
        if (map->count > SIZE_MAX / 2) return nullptr;
        const th_type **from = core_alloc_array(const th_type *,
                                               2 * map->count);
        th_type **to = core_alloc_array(th_type *, 2 * map->count);
        if (!from || !to) {
            core_free(from);
            core_free(to);
            return nullptr;
        }
        for (size_t i = 0; i < map->count; i++) {
            from[2 * i] = map->body[i];
            from[2 * i + 1] = th_resolve(map->body[i]);
            to[2 * i] = map->declaration[i];
            to[2 * i + 1] = map->declaration[i];
        }
        th_type *result = th_type_clone_replacing(
            ir_module_types(index->module), type, from, to, 2 * map->count);
        core_free(from);
        core_free(to);
        return result;
    }
    return type;
}

th_type *
n00bb_abi_type_index_symbol_type(n00bb_abi_type_index_t *index,
                                  const parse_sym_entry_t *symbol)
{
    if (!index || !symbol) return nullptr;
    bool found = false;
    th_type *type = core_dict_get(&index->symbol_types, symbol, &found);
    return found ? type : nullptr;
}

bool
n00bb_abi_type_index_collect_symbols(n00bb_abi_type_index_t *index,
                                       const parse_symtab_t *symbols,
                                       n00bb_abi_symbol_selector_fn select,
                                       void *context)
{
    if (!index || !symbols || index->failed) return false;
    for (const parse_scope_t *scope = symbols->all_scopes;
         scope; scope = scope->all_next) {
        for (const parse_sym_entry_t *symbol = scope->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            if (!n00bb_abi_decl_owned_symbol(symbol)) continue;
            bool include = true;
            if (select && !select(context, symbol, &include)) return false;
            if (!include) continue;
            th_type *type = declaration_type(index, symbol, symbol->type);
            if (!type || !add_type(index, type, nullptr)) return false;
            core_dict_put(&index->symbol_types, (void *)symbol, type);
        }
    }
    return true;
}
