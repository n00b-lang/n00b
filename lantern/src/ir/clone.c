#include <string.h>

#include "private/ir/internal.h"
#include "ir/shape.h"

typedef struct {
    ir_func_t *source;
    ir_func_t *copy;
    th_type_cloner_t *cloner;
    const char **failure;
} clone_t;

static th_type *clone_type(clone_t *c, th_type *type)
{
    if (!type) return nullptr;
    th_type *result = th_type_cloner_clone(c->cloner, type);
    if (!result) {
        if (c->failure) *c->failure = "type substitution";
        return nullptr;
    }
    result = ir_type_intern(c->copy->owner, result);
    if (!result || th_kind_of(th_resolve(type)) != TH_NAMED
        || result == type) return result;
    th_type *definition = ir_type_definition(c->source->owner, type);
    if (!definition) return result;
    th_type *bound = th_type_cloner_clone(c->cloner, definition);
    if (!bound) {
        if (c->failure) *c->failure = "record definition substitution";
        return nullptr;
    }
    if (!ir_type_define(c->copy->owner, result, bound)) {
        if (c->failure) *c->failure = "record definition conflict";
        return nullptr;
    }
    return result;
}

static ir_value_t *clone_value(clone_t *c, ir_value_t *value)
{
    if (!value || value->owner != c->source) return value;
    switch (value->kind) {
    case IR_VAL_PARAM:
        return value->index < c->copy->n_params
            ? c->copy->params[value->index] : nullptr;
    case IR_VAL_LOCAL:
        return value->local && value->local->index < c->copy->n_locals
            ? ir_local_value(c->copy,
                c->copy->locals[value->local->index]) : nullptr;
    case IR_VAL_INSTR:
        if (!value->def || !value->def->block
            || value->def->block->index >= c->copy->n_blocks) return nullptr;
        {
            ir_block_t *block = c->copy->blocks[value->def->block->index];
            return value->def->index < block->n_instrs
                ? block->instrs[value->def->index]->dest : nullptr;
        }
    case IR_VAL_FUNC:
        {
            th_type *type = clone_type(c, value->type);
            if (!type) return nullptr;
            if (th_kind_of(th_resolve(type)) == TH_INDIRECTION)
                return ir_func_reference(c->copy->owner, c->copy);
            return ir_func_address(c->copy->owner, c->copy);
        }
    case IR_VAL_GLOBAL:
        return value;
    }
    return nullptr;
}

static ir_shape_t *clone_shape(clone_t *c, ir_shape_t *source)
{
    if (!source) return nullptr;
    th_type *element = clone_type(c, ir_shape_element_type(source));
    uint32_t rank = ir_shape_rank(source);
    ir_shape_t *copy = element && rank
        ? ir_shape_new(c->copy->owner->ctx, element, rank) : nullptr;
    if (!copy) return nullptr;
    for (uint32_t i = 0; i < rank; i++) {
        ir_shape_extent_t extent;
        ir_shape_measure_t stride;
        if (!ir_shape_extent(source, i, &extent)
            || !ir_shape_stride(source, i, &stride)) return nullptr;
        bool has_extent_value = extent.value != nullptr;
        bool has_stride_value = stride.value != nullptr;
        if (has_extent_value) extent.value = clone_value(c, extent.value);
        if (has_stride_value) stride.value = clone_value(c, stride.value);
        if ((has_extent_value && !extent.value)
            || (has_stride_value && !stride.value)
            || !ir_shape_set_extent(copy, i, extent)
            || !ir_shape_set_stride(copy, i, stride)) return nullptr;
    }
    ir_shape_measure_t count;
    ir_shape_storage_t storage;
    if (!ir_shape_element_count(source, &count)
        || !ir_shape_storage(source, &storage)) return nullptr;
    bool has_count_value = count.value != nullptr;
    if (has_count_value) count.value = clone_value(c, count.value);
    if (has_count_value && !count.value) return nullptr;
    return ir_shape_set_element_count(copy, count)
        && ir_shape_set_storage(copy, storage.flat_prefix_rank,
            storage.classification_known) ? copy : nullptr;
}

static bool clone_annots(clone_t *c, ir_annots_t *destination,
                         const ir_annots_t *source)
{
    for (uint32_t i = 0; i < source->n; i++) {
        ir_annot_t annotation = source->a[i];
        if (!strncmp(annotation.key, "ir.alias", 8)
            || !strncmp(annotation.key, "ir.aliased_by", 13)
            || !strncmp(annotation.key, "ir.lifetime", 11)) continue;
        if (c->failure) *c->failure = annotation.key;
        if (!strcmp(annotation.key, "ir.callable_contract")) continue;
        if (annotation.kind == IR_ANNOT_TYPE) {
            annotation.t = clone_type(c, annotation.t);
            if (!annotation.t) return false;
        } else if (annotation.kind == IR_ANNOT_VALUE && annotation.v) {
            annotation.v = clone_value(c, annotation.v);
            if (!annotation.v) return false;
        }
        if (!ir_annot_set(c->copy->owner->ctx, destination, annotation))
            return false;
    }
    return true;
}

static bool clone_value_details(clone_t *c, ir_value_t *source,
                                ir_value_t *copy)
{
    copy->is_keyword = source->is_keyword;
    copy->is_rest = source->is_rest;
    copy->is_optional = source->is_optional;
    if (c->failure) *c->failure = "value shape";
    copy->shape = source->shape ? clone_shape(c, source->shape) : nullptr;
    return (!source->shape || copy->shape)
        && clone_annots(c, &copy->annots, &source->annots);
}

static bool clone_operand(clone_t *c, ir_operand_t *operand)
{
    switch (operand->kind) {
    case IR_OPD_VALUE:
    case IR_OPD_SPREAD:
    case IR_OPD_FRESH:
        operand->value = clone_value(c, operand->value);
        return operand->value != nullptr;
    case IR_OPD_TYPE:
        operand->type = clone_type(c, operand->type);
        return operand->type != nullptr;
    case IR_OPD_BLOCK:
    case IR_OPD_DEFAULT_BLOCK:
        if (!operand->block || operand->block->owner != c->source)
            return false;
        operand->block = c->copy->blocks[operand->block->index];
        return true;
    case IR_OPD_PHI_ARM:
        if (!operand->phi.block
            || operand->phi.block->owner != c->source) return false;
        operand->phi.value = clone_value(c, operand->phi.value);
        operand->phi.block = c->copy->blocks[operand->phi.block->index];
        return operand->phi.value != nullptr;
    case IR_OPD_SWITCH_ARM:
        if (!operand->arm.block
            || operand->arm.block->owner != c->source) return false;
        operand->arm.block = c->copy->blocks[operand->arm.block->index];
        return true;
    case IR_OPD_NAMED_ARG:
        operand->named.value = clone_value(c, operand->named.value);
        return operand->named.value != nullptr;
    default:
        return true;
    }
}

ir_func_t *ir_func_clone_replacing(ir_func_t *source, const char *name,
                                  const th_type *const *from,
                                  th_type *const *to, size_t count,
                                  const char **failure)
{
    return ir_func_clone_into(source, source ? source->owner : nullptr, name,
                              from, to, count, failure);
}

static ir_func_t *clone_function(ir_func_t *source, ir_module_t *module,
                                 const char *name, th_type_cloner_t *cloner,
                                 const char **failure)
{
    clone_t c = {.source = source, .cloner = cloner, .failure = failure};
    if (failure) *failure = "function signature";
    th_type *signature = th_type_cloner_clone(cloner, source->type);
    if (!signature) return nullptr;
    c.copy = ir_func_add(module, name, signature, IR_LINK_INTERNAL);
    if (!c.copy) return nullptr;
    c.copy->span = source->span;
    c.copy->origin = source->origin;
    if (failure) *failure = "parameters";
    for (uint32_t i = 0; i < source->n_params; i++) {
        ir_value_t *original = source->params[i];
        th_type *type = clone_type(&c, original->type);
        if (!type || !ir_param_add(c.copy, original->name, type))
            return nullptr;
    }
    if (failure) *failure = "locals";
    for (uint32_t i = 0; i < source->n_locals; i++) {
        ir_local_t *original = source->locals[i];
        th_type *type = clone_type(&c, original->type);
        if (!type || !ir_local_add(c.copy, original->name, type))
            return nullptr;
        if (original->value && !ir_local_value(c.copy, c.copy->locals[i]))
            return nullptr;
    }
    if (failure) *failure = "blocks";
    for (uint32_t b = 0; b < source->n_blocks; b++) {
        ir_block_t *original = source->blocks[b];
        if (!ir_block_add(c.copy, original->name)) return nullptr;
    }
    if (failure) *failure = "instruction destinations";
    for (uint32_t b = 0; b < source->n_blocks; b++) {
        ir_block_t *original = source->blocks[b];
        ir_block_t *copy = c.copy->blocks[b];
        for (uint32_t i = 0; i < original->n_instrs; i++) {
            ir_instr_t *old = original->instrs[i];
            ir_instr_t *new = ir_instr_add(copy, old->op);
            if (!new) return nullptr;
            if (old->dest) {
                th_type *type = clone_type(&c, old->dest->type);
                if (!type) return nullptr;
                if (!ir_instr_set_dest(new, old->dest->name, type)) {
                    if (failure) *failure = "instruction destination rejected";
                    return nullptr;
                }
            } else if (old->dest_local) {
                if (!ir_instr_set_dest_local(new,
                    c.copy->locals[old->dest_local->index])) return nullptr;
            }
        }
    }
    if (failure) *failure = "function metadata";
    if (!ir_func_set_callable_contract(c.copy, c.copy->type)) return nullptr;
    if (!clone_annots(&c, &c.copy->annots, &source->annots)) return nullptr;
    if (failure) *failure = "parameter metadata";
    for (uint32_t i = 0; i < source->n_params; i++) {
        if (failure) *failure = "optional parameter";
        if (ir_func_parameter_is_optional(source, i)
            && !ir_func_set_parameter_optional(c.copy, i, true))
            return nullptr;
        if (!clone_value_details(&c, source->params[i], c.copy->params[i]))
            return nullptr;
    }
    if (failure) *failure = "local metadata";
    for (uint32_t i = 0; i < source->n_locals; i++) {
        ir_local_t *old = source->locals[i];
        ir_local_t *new = c.copy->locals[i];
        new->mutability = old->mutability;
        new->address_taken = old->address_taken;
        new->has_init = old->has_init;
        new->init = old->init;
        new->init_f = old->init_f;
        new->init_text = old->init_text;
        new->init_is_float = old->init_is_float;
        new->span = old->span;
        new->origin = old->origin;
        new->shape = old->shape ? clone_shape(&c, old->shape) : nullptr;
        if ((old->shape && !new->shape)
            || !clone_annots(&c, &new->annots, &old->annots)) return nullptr;
        if (old->value && !clone_value_details(&c, old->value, new->value))
            return nullptr;
    }
    if (failure) *failure = "instruction metadata or operands";
    for (uint32_t b = 0; b < source->n_blocks; b++) {
        ir_block_t *old_block = source->blocks[b];
        ir_block_t *new_block = c.copy->blocks[b];
        new_block->span = old_block->span;
        new_block->origin = old_block->origin;
        if (!clone_annots(&c, &new_block->annots, &old_block->annots))
            return nullptr;
        for (uint32_t i = 0; i < old_block->n_instrs; i++) {
            ir_instr_t *old = old_block->instrs[i];
            ir_instr_t *new = new_block->instrs[i];
            new->span = old->span;
            new->origin = old->origin;
            new->shape_constraint = old->shape_constraint
                ? clone_shape(&c, old->shape_constraint) : nullptr;
            if ((old->shape_constraint && !new->shape_constraint)
                || !clone_annots(&c, &new->annots, &old->annots))
                return nullptr;
            if (old->dest && !clone_value_details(&c, old->dest, new->dest))
                return nullptr;
            if (old->const_bytes && !ir_instr_set_const_bytes(new,
                old->const_bytes, old->const_byte_len)) return nullptr;
            for (uint32_t j = 0; j < old->n_mods; j++)
                if (!ir_instr_add_mod(new, old->mods[j])) return nullptr;
            if (old->n_operands) {
                new->operands = ir_alloc(c.copy->owner->ctx,
                    old->n_operands * sizeof(*new->operands));
                if (!new->operands) return nullptr;
                memcpy(new->operands, old->operands,
                    old->n_operands * sizeof(*new->operands));
                new->n_operands = new->cap_operands = old->n_operands;
                for (uint32_t j = 0; j < new->n_operands; j++)
                    if (!clone_operand(&c, &new->operands[j])) return nullptr;
            }
        }
    }
    if (failure) *failure = nullptr;
    return c.copy;
}

ir_func_t *ir_func_clone_into(ir_func_t *source, ir_module_t *module,
                              const char *name, const th_type *const *from,
                              th_type *const *to, size_t count,
                              const char **failure)
{
    if (failure) *failure = "invalid clone request";
    if (!source || !module || module->ctx != source->owner->ctx || !name
        || !*name || (count && (!from || !to)))
        return nullptr;
    th_type_cloner_t *cloner = th_type_cloner_new(
        ir_ctx_types(source->owner->ctx), from, to, count);
    if (!cloner) return nullptr;
    ir_func_t *copy = clone_function(source, module, name, cloner, failure);
    th_type_cloner_free(cloner);
    return copy;
}
