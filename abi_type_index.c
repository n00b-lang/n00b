#include "abi_type_index.h"

#include <limits.h>

#include "ir/typelang.h"
#include "lib/alloc.h"
#include "lib/indexed_table.h"
#include "lib/list.h"

typedef struct {
    uint64_t id;
} type_key_t;

typedef type_key_t *type_key_ptr_t;
core_list_decl(type_key_ptr_t);

struct n00bb_abi_type_index_t {
    ir_module_t *module;
    core_indexed_table_t *table;
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

static bool
add_optional(n00bb_abi_type_index_t *index, th_type *type)
{
    return !type || add_type(index, type, nullptr);
}

static bool
add_annotations(n00bb_abi_type_index_t *index, ir_annots_t *annotations)
{
    if (!annotations) return true;
    for (uint32_t i = 0; i < ir_annot_count(annotations); i++) {
        ir_annot_t *annotation = ir_annot_at(annotations, i);
        if (!annotation) return false;
        if (annotation->kind == IR_ANNOT_TYPE
            && !add_optional(index, annotation->t)) return false;
        if (annotation->kind == IR_ANNOT_VALUE && annotation->v
            && !add_optional(index, annotation->v->type)) return false;
    }
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
            && add_optional(index, constraint.method.call);
    case TH_CON_IMPLEMENTS:
    case TH_CON_KIND:
        return true;
    }
    return false;
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
        return add_optional(index, th_seq_elem(type));
    case TH_RECORD:
        for (size_t i = 0; i < th_record_len(type); i++) {
            const th_member *member = th_record_member(type, i);
            if (!member || !add_optional(index, member->type)) return false;
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
add_type(n00bb_abi_type_index_t *index, th_type *type,
         uint32_t *out_index)
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
    if (!add_children(index, type)) {
        index->failed = true;
        return false;
    }
    return true;
}

n00bb_abi_type_index_t *
n00bb_abi_type_index_new(ir_module_t *module)
{
    if (!module || !ir_module_types(module)) return nullptr;
    n00bb_abi_type_index_t *index = core_alloc(n00bb_abi_type_index_t);
    index->module = module;
    index->table = core_indexed_table_new(1, UINT32_MAX >> 1,
                                          type_key_hash, type_key_equal);
    th_type *nil = th_name(ir_module_types(module), "nil");
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

bool
n00bb_abi_type_index_collect_ir(n00bb_abi_type_index_t *index,
                                 ir_module_t *module)
{
    if (!index || !module || index->failed
        || ir_module_types(module) != ir_module_types(index->module))
        return false;
    if (!add_annotations(index, ir_module_annots(module))) return false;
    for (size_t i = 0; i < ir_type_count(module); i++)
        if (!add_optional(index, ir_type_at(module, i))) return false;
    for (size_t i = 0; i < ir_global_count(module); i++) {
        ir_global_t *global = ir_global_at(module, i);
        if (!global || !add_optional(index, global->type)
            || !add_annotations(index, &global->annots)) return false;
    }
    for (size_t i = 0; i < ir_func_count(module); i++) {
        ir_func_t *function = ir_func_at(module, i);
        if (!function || !add_optional(index, function->type)
            || !add_annotations(index, &function->annots)) return false;
        if (!add_optional(index, ir_func_callable_contract(function)))
            return false;
        for (uint32_t p = 0; p < function->n_params; p++)
            if (!function->params[p]
                || !add_optional(index, function->params[p]->type)
                || !add_annotations(index, &function->params[p]->annots))
                return false;
        for (uint32_t l = 0; l < function->n_locals; l++)
            if (!function->locals[l]
                || !add_optional(index, function->locals[l]->type)
                || !add_annotations(index, &function->locals[l]->annots))
                return false;
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            if (!block || !add_annotations(index, &block->annots)) return false;
            for (uint32_t j = 0; j < block->n_instrs; j++) {
                ir_instr_t *instruction = block->instrs[j];
                if (!instruction || (instruction->dest
                    && (!add_optional(index, instruction->dest->type)
                        || !add_annotations(index,
                            &instruction->dest->annots)))
                    || !add_annotations(index, &instruction->annots))
                    return false;
                for (uint32_t o = 0; o < instruction->n_operands; o++) {
                    ir_operand_t *operand = &instruction->operands[o];
                    ir_value_t *value = ir_operand_value(operand);
                    if ((value && !add_optional(index, value->type))
                        || (value && !add_annotations(index, &value->annots))
                        || (operand->kind == IR_OPD_TYPE
                            && !add_optional(index, operand->type)))
                        return false;
                }
            }
        }
    }
    return true;
}

bool
n00bb_abi_type_index_collect_symbols(n00bb_abi_type_index_t *index,
                                       const parse_symtab_t *symbols)
{
    if (!index || !symbols || index->failed) return false;
    for (const parse_scope_t *scope = symbols->all_scopes;
         scope; scope = scope->all_next) {
        for (const parse_sym_entry_t *symbol = scope->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            if (!symbol->decl_node && !symbol->definition_count
                && !symbol->use_count) continue;
            if (!add_optional(index, symbol->type)
                || !add_optional(index, symbol->error_type)
                || !add_optional(index, symbol->callable_contract))
                return false;
        }
    }
    return true;
}
