#include "storage_placement.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/pass.h"
#include "lib/alloc.h"
#include "ir/typelang.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"

typedef struct {
    ir_instr_t *allocation;
    bool        local;
} placed_alloc_t;
core_list_decl(placed_alloc_t);

typedef ir_block_t *placed_block_ptr_t;
core_list_decl(placed_block_ptr_t);

static ir_instr_t *
placed_instruction(ir_block_t *block, uint32_t at, ir_span_t span, ir_op_t op,
                   th_type *type)
{
    ir_instr_t *instruction = ir_instr_generated(block, op, span,
        CORE_STRING_STATIC("storage_placement"), at);
    if (!instruction || !type) return instruction;
    return ir_instr_set_fresh_dest(instruction, "__n00bb_placed", type)
        ? instruction : nullptr;
}

static const ir_value_t *
addressed_local(const ir_instr_t *in)
{
    return in->op == IR_OP_ADDRESS_OF && in->dest && in->n_operands == 1
        && in->operands[0].kind == IR_OPD_VALUE ? in->operands[0].value
                                                : nullptr;
}

static bool
local_escapes(ir_summaries_t *summaries, ir_func_t *function,
              const ir_local_t *local)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (addressed_local(in) == local->value
                && ir_address_escapes(summaries, function, in->dest))
                return true;
        }
    }
    return false;
}

static void
escaping_locals(ir_summaries_t *summaries, ir_func_t *function,
                core_dict_t *out)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            const ir_value_t *local = addressed_local(in);
            if (local && !core_dict_contains(out, (void *)local)
                && ir_address_escapes(summaries, function, in->dest))
                core_dict_put(out, local, 1);
        }
    }
}

bool
n00bb_binding_escapes(ir_summaries_t *summaries, ir_func_t *function,
                      const ir_value_t *binding)
{
    if (!binding) return false;
    if (binding->kind == IR_VAL_LOCAL)
        return binding->local && binding->local->address_taken
            && local_escapes(summaries, function, binding->local);
    return th_kind_of(th_resolve(binding->type)) == TH_INDIRECTION
        && ir_address_escapes(summaries, function, binding);
}

static ir_value_t *
fresh_storage(ir_block_t *block, uint32_t at, ir_span_t span,
              const ir_local_t *local, ir_local_t *holder,
              th_type *storage_type)
{
    ir_instr_t *allocation = placed_instruction(block, at, span, IR_OP_ALLOC,
                                                storage_type);
    ir_instr_t *record = allocation
        && ir_instr_operand_type(allocation, local->type)
        ? ir_instr_generated(block, IR_OP_COPY, span,
              CORE_STRING_STATIC("storage_placement"), at + 1)
        : nullptr;
    return record && ir_instr_set_dest_local(record, holder)
        && ir_instr_operand_value(record, allocation->dest)
        ? allocation->dest : nullptr;
}

static bool
read_through(ir_instr_t *in, uint32_t operand, ir_value_t *held,
             const ir_local_t *local, th_type *storage_type)
{
    ir_instr_t *address = placed_instruction(in->block, in->index, in->span,
                                             IR_OP_COPY, storage_type);
    if (!address || !ir_instr_operand_value(address, held)) return false;
    ir_instr_t *load = placed_instruction(in->block, in->index, in->span,
                                          IR_OP_LOAD, local->type);
    if (!load || !ir_instr_operand_value(load, address->dest)) return false;
    in->operands[operand].value = load->dest;
    ir_instr_use_added(in, operand);
    return true;
}

static ir_instr_t *
construction_allocation(const ir_instr_t *in, const ir_local_t *local)
{
    ir_value_t *loaded = in->op == IR_OP_COPY && in->n_operands == 1
        ? ir_operand_value(&in->operands[0]) : nullptr;
    ir_instr_t *load = loaded && loaded->kind == IR_VAL_INSTR
        ? loaded->def : nullptr;
    ir_value_t *address = load && load->op == IR_OP_LOAD
        && load->n_operands == 1 ? ir_operand_value(&load->operands[0])
        : nullptr;
    ir_instr_t *allocation = address && address->kind == IR_VAL_INSTR
        ? address->def : nullptr;
    ir_func_t *function = in->block->owner;
    if (!allocation || allocation->op != IR_OP_ALLOC
        || allocation->block != in->block || allocation->n_operands != 1
        || !ir_type_eq(function->owner, th_ind_referent(address->type),
                       local->type)) return nullptr;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *user = block->instrs[i];
            for (uint32_t k = 0; k < user->n_operands; k++) {
                const ir_operand_t *operand = &user->operands[k];
                if (operand->kind != IR_OPD_VALUE
                    && operand->kind != IR_OPD_FRESH) continue;
                if (operand->value == loaded && user != in) return nullptr;
                if (operand->value == address && user != load
                    && operand->kind != IR_OPD_FRESH) return nullptr;
            }
        }
    }
    return allocation;
}

static ir_instr_t *
construct_in_place(ir_instr_t *in, ir_instr_t *allocation,
                   ir_local_t *holder, th_type *storage_type)
{
    ir_instr_t *load = ir_operand_value(&in->operands[0])->def;
    allocation->dest->type = storage_type;
    ir_instr_t *record = ir_instr_generated(allocation->block, IR_OP_COPY,
        allocation->span, CORE_STRING_STATIC("storage_placement"),
        allocation->index + 1);
    return record && ir_instr_set_dest_local(record, holder)
        && ir_instr_operand_value(record, allocation->dest)
        && ir_instr_erase(in) && ir_instr_erase(load) ? record : nullptr;
}

static ir_instr_t *
write_through(ir_instr_t *in, ir_local_t *holder, ir_value_t *held,
              const ir_local_t *local, th_type *storage_type)
{
    bool initializes = ir_instr_is_binding_initialization(in);
    ir_instr_t *allocation = initializes
        ? construction_allocation(in, local) : nullptr;
    if (allocation)
        return construct_in_place(in, allocation, holder, storage_type);
    if (!ir_instr_set_fresh_dest(in, "__n00bb_placed", local->type)
        || (initializes && !ir_instr_set_binding_initialization(in, false)))
        return nullptr;
    ir_block_t *block = in->block;
    uint32_t after = in->index + 1;
    ir_value_t *address = nullptr;
    if (initializes) {
        address = fresh_storage(block, after, in->span, local, holder,
                                storage_type);
        after += 2;
    }
    else {
        ir_instr_t *copy = placed_instruction(block, after, in->span,
                                              IR_OP_COPY, storage_type);
        address = copy && ir_instr_operand_value(copy, held) ? copy->dest
                                                            : nullptr;
        after += 1;
    }
    ir_instr_t *store = address ? ir_instr_generated(block, IR_OP_STORE,
        in->span, CORE_STRING_STATIC("storage_placement"), after) : nullptr;
    return store && ir_instr_operand_value(store, address)
        && ir_instr_operand_value(store, in->dest)
        && (!initializes
            || ir_instr_set_binding_initialization(store, true))
        ? store : nullptr;
}

static bool
move_local_to_storage(ir_func_t *function, th_ctx *types, ir_local_t *local)
{
    th_type *storage_type = th_ind(types, 0, local->type);
    core_buffer_t name = {0};
    core_buffer_printf(&name, "__n00bb_storage_of_%.*s",
                       (int)local->name.u8_bytes, local->name.data);
    ir_local_t *holder = storage_type
        ? ir_local_add(function, core_buffer_string(&name), storage_type)
        : nullptr;
    core_buffer_release(&name);
    ir_value_t *held = holder ? ir_local_value(function, holder) : nullptr;
    ir_value_t *target = local->value;
    if (!held || !target || !function->n_blocks
        || !fresh_storage(function->blocks[0], 0, function->span, local,
                          holder, storage_type))
        return false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            for (uint32_t k = 0; k < in->n_operands; k++) {
                if (in->operands[k].kind != IR_OPD_VALUE
                    || in->operands[k].value != target) continue;
                if (in->op == IR_OP_ADDRESS_OF) {
                    in->op = IR_OP_COPY;
                    in->operands[k].value = held;
                    ir_instr_use_added(in, k);
                    continue;
                }
                if (!read_through(in, k, held, local, storage_type))
                    return false;
            }
            i = in->index;
            if (in->dest_kind == IR_DEST_LOCAL && in->dest_local == local) {
                ir_instr_t *store = write_through(in, holder, held, local,
                                                  storage_type);
                if (!store) return false;
                i = store->index;
            }
        }
    }
    local->address_taken = false;
    return true;
}

static void
release_scope_finalizer(ir_func_t *function, ir_value_t *binding)
{
    ir_annots_t *annots = binding ? ir_value_annots(binding) : nullptr;
    ir_annot_t *owned = annots ? ir_annot_get(annots,
        CORE_STRING_STATIC("n00bb.finalize_owned")) : nullptr;
    ir_local_t *flag = owned && owned->kind == IR_ANNOT_VALUE && owned->v
        && owned->v->kind == IR_VAL_LOCAL ? owned->v->local : nullptr;
    for (uint32_t b = 0; flag && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (in->op != IR_OP_COPY || in->dest_kind != IR_DEST_LOCAL
                || in->dest_local != flag || !in->n_operands) continue;
            ir_value_t *source = ir_operand_value(&in->operands[0]);
            ir_instr_t *constant = source && source->kind == IR_VAL_INSTR
                ? source->def : nullptr;
            if (constant && constant->op == IR_OP_CONST
                && constant->n_operands == 1
                && constant->operands[0].kind == IR_OPD_CONST)
                constant->operands[0].lit.i.mag = 0;
        }
    }
}

static bool
retire_scope_finalizer(ir_ctx_t *context, ir_value_t *binding)
{
    ir_annots_t *annots = binding ? ir_value_annots(binding) : nullptr;
    return !annots || !ir_annot_get(annots,
            CORE_STRING_STATIC("n00bb.finalize_owned"))
        || ir_annot_set(context, annots, (ir_annot_t){
            .key = CORE_STRING_STATIC("n00bb.finalize_owned"),
            .kind = IR_ANNOT_BOOL});
}

static bool
retire_scope_finalizers(ir_ctx_t *context, ir_func_t *function)
{
    bool ok = true;
    for (uint32_t p = 0; ok && p < function->n_params; p++)
        ok = retire_scope_finalizer(context, function->params[p]);
    for (uint32_t l = 0; ok && l < function->n_locals; l++)
        ok = retire_scope_finalizer(context, function->locals[l]->value);
    for (uint32_t b = 0; ok && b < function->n_blocks; b++)
        for (uint32_t i = 0; ok && i < function->blocks[b]->n_instrs; i++)
            ok = retire_scope_finalizer(context,
                                        function->blocks[b]->instrs[i]->dest);
    return ok;
}

static bool
block_in_cycle(ir_block_t *block)
{
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_t(placed_block_ptr_t) work = core_list_new(placed_block_ptr_t);
    core_list_push(work, block);
    bool cycle = false;
    while (!cycle && work.len) {
        ir_block_t *at = work.data[--work.len];
        ir_instr_t *last = at->n_instrs ? at->instrs[at->n_instrs - 1]
                                        : nullptr;
        for (uint32_t s = 0; last && !cycle && s < ir_succ_count(last); s++) {
            ir_block_t *next = ir_succ_at(last, s);
            cycle = next == block;
            if (next && !cycle && !core_dict_contains(&seen, next)) {
                core_dict_put(&seen, next, next);
                core_list_push(work, next);
            }
        }
    }
    core_list_free(work);
    core_dict_free(&seen);
    return cycle;
}

static bool
annotate_allocations(ir_ctx_t *context, ir_summaries_t *summaries,
                     ir_func_t *function)
{
    core_list_t(placed_alloc_t) found = core_list_new(placed_alloc_t);
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *allocation = block->instrs[i];
            bool required = false;
            if (allocation->op != IR_OP_ALLOC || !allocation->dest
                || ir_instr_storage_class(allocation, &required).data
                || required) continue;
            core_list_push(found, ((placed_alloc_t){
                .allocation = allocation,
                .local      = !ir_address_escapes(summaries, function,
                                                  allocation->dest)
                    && !block_in_cycle(block)}));
        }
    }
    bool ok = true;
    for (size_t a = 0; a < found.len; a++)
        if (!found.data[a].local)
            release_scope_finalizer(function, found.data[a].allocation->dest);
    for (size_t a = 0; ok && a < found.len; a++)
        ok = ir_annot_set(context, &found.data[a].allocation->annots,
            (ir_annot_t){.key  = CORE_STRING_STATIC("ir.allocation_local"),
                         .kind = IR_ANNOT_BOOL, .b = found.data[a].local});
    core_list_free(found);
    if (!ok) return false;
    return true;
}

bool
n00bb_place_storage(ir_ctx_t *context, ir_summaries_t *summaries,
                    ir_module_t *module)
{
    th_ctx *types = ir_module_types(module);
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        if (!function || !function->n_blocks) continue;
        uint32_t locals = function->n_locals;
        ir_local_t **moved = core_alloc_array(ir_local_t *, locals + 1);
        uint32_t count = 0;
        size_t scope = ir_analysis_scope_begin();
        core_dict_t escaping;
        core_dict_init(&escaping, nullptr, nullptr);
        escaping_locals(summaries, function, &escaping);
        for (uint32_t l = 0; l < locals; l++) {
            ir_local_t *local = function->locals[l];
            if (!local->address_taken
                || !core_dict_contains(&escaping, local->value)) continue;
            moved[count++] = local;
        }
        core_dict_free(&escaping);
        ir_analysis_scope_end(scope);
        bool ok = true;
        for (uint32_t m = 0; ok && m < count; m++) {
            release_scope_finalizer(function, moved[m]->value);
            ok = move_local_to_storage(function, types, moved[m]);
        }
        core_free(moved);
        if (!ok) return false;
        if (!annotate_allocations(context, summaries, function)
            || !retire_scope_finalizers(context, function))
            return false;
    }
    return true;
}
