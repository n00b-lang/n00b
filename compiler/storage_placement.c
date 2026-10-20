#include "storage_placement.h"
#include "ir/doms.h"
#include "ir/pass.h"
#include "lib/alloc.h"
#include "ir/typelang.h"
#include "lib/buffer.h"

typedef struct {
    ir_func_t *function;
    uint32_t   counter;
} placement_t;

static ir_instr_t *
placed_instruction(placement_t *placement, ir_block_t *block, uint32_t at,
                   ir_span_t span, ir_op_t op, th_type *type)
{
    ir_instr_t *instruction = ir_instr_generated(block, op, span,
        CORE_STRING_STATIC("storage_placement"), at);
    if (!instruction || !type) return instruction;
    for (;;) {
        core_buffer_t name = {0};
        core_buffer_printf(&name, "__n00bb_placed_%u", placement->counter++);
        core_string_t text = core_buffer_string(&name);
        bool taken = ir_value_find(placement->function, text) != nullptr;
        bool bound = !taken && ir_instr_set_dest(instruction, text, type);
        core_buffer_release(&name);
        if (!taken) return bound ? instruction : nullptr;
    }
}

static bool
local_escapes(ir_summaries_t *summaries, ir_func_t *function,
              const ir_local_t *local)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (in->op == IR_OP_ADDRESS_OF && in->dest && in->n_operands == 1
                && in->operands[0].kind == IR_OPD_VALUE
                && in->operands[0].value == local->value
                && ir_address_escapes(summaries, function, in->dest))
                return true;
        }
    }
    return false;
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

static bool
initialized_before(ir_doms_t *doms, const ir_func_t *function,
                   const ir_local_t *local, const ir_instr_t *use)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        if (block != use->block && !ir_dominates(doms, block, use->block))
            continue;
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            if (block == use->block && i >= use->index) break;
            if (in->dest_kind == IR_DEST_LOCAL && in->dest_local == local
                && ir_instr_is_binding_initialization(in)) return true;
        }
    }
    return false;
}

static bool
needs_entry_storage(ir_doms_t *doms, const ir_func_t *function,
                    const ir_local_t *local)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            bool uses = in->dest_kind == IR_DEST_LOCAL
                && in->dest_local == local
                && !ir_instr_is_binding_initialization(in);
            for (uint32_t k = 0; !uses && k < in->n_operands; k++)
                uses = in->operands[k].kind == IR_OPD_VALUE
                    && in->operands[k].value == local->value;
            if (uses && !initialized_before(doms, function, local, in))
                return true;
        }
    }
    return false;
}

static ir_value_t *
fresh_storage(placement_t *placement, ir_block_t *block, uint32_t at,
              ir_span_t span, const ir_local_t *local, ir_local_t *holder,
              th_type *storage_type)
{
    ir_instr_t *allocation = placed_instruction(placement, block, at, span,
                                                IR_OP_ALLOC, storage_type);
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
read_through(placement_t *placement, ir_instr_t *in, uint32_t operand,
             ir_value_t *held, const ir_local_t *local, th_type *storage_type)
{
    ir_instr_t *address = placed_instruction(placement, in->block,
        in->index, in->span, IR_OP_COPY, storage_type);
    if (!address || !ir_instr_operand_value(address, held)) return false;
    ir_instr_t *load = placed_instruction(placement, in->block, in->index,
                                          in->span, IR_OP_LOAD, local->type);
    if (!load || !ir_instr_operand_value(load, address->dest)) return false;
    in->operands[operand].value = load->dest;
    return true;
}

static bool
rename_dest(placement_t *placement, ir_instr_t *in, th_type *type)
{
    for (;;) {
        core_buffer_t name = {0};
        core_buffer_printf(&name, "__n00bb_placed_%u", placement->counter++);
        core_string_t text = core_buffer_string(&name);
        bool taken = ir_value_find(placement->function, text) != nullptr;
        bool bound = !taken && ir_instr_set_dest(in, text, type);
        core_buffer_release(&name);
        if (!taken) return bound;
    }
}

static ir_instr_t *
write_through(placement_t *placement, ir_instr_t *in, ir_local_t *holder,
              ir_value_t *held, const ir_local_t *local, th_type *storage_type)
{
    bool initializes = ir_instr_is_binding_initialization(in);
    if (!rename_dest(placement, in, local->type)
        || (initializes && !ir_instr_set_binding_initialization(in, false)))
        return nullptr;
    ir_block_t *block = in->block;
    uint32_t after = in->index + 1;
    ir_value_t *address = nullptr;
    if (initializes) {
        address = fresh_storage(placement, block, after, in->span, local,
                                holder, storage_type);
        after += 2;
    }
    else {
        ir_instr_t *copy = placed_instruction(placement, block, after,
            in->span, IR_OP_COPY, storage_type);
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
move_local_to_storage(placement_t *placement, th_ctx *types,
                      ir_local_t *local, bool entry_storage)
{
    ir_func_t *function = placement->function;
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
        || (entry_storage
            && !fresh_storage(placement, function->blocks[0], 0,
                              function->span, local, holder, storage_type)))
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
                    continue;
                }
                if (!read_through(placement, in, k, held, local,
                                  storage_type)) return false;
            }
            i = in->index;
            if (in->dest_kind == IR_DEST_LOCAL && in->dest_local == local) {
                ir_instr_t *store = write_through(placement, in, holder,
                                                  held, local, storage_type);
                if (!store) return false;
                i = store->index;
            }
        }
    }
    local->address_taken = false;
    return true;
}

static bool
annotate_allocations(ir_ctx_t *context, ir_summaries_t *summaries,
                     ir_func_t *function)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *allocation = block->instrs[i];
            bool required = false;
            if (allocation->op != IR_OP_ALLOC || !allocation->dest
                || ir_instr_storage_class(allocation, &required).data
                || required) continue;
            bool local = !ir_address_escapes(summaries, function,
                                             allocation->dest);
            if (!ir_annot_set(context, &allocation->annots, (ir_annot_t){
                    .key = CORE_STRING_STATIC("ir.allocation_local"),
                    .kind = IR_ANNOT_BOOL, .b = local})) return false;
        }
    }
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
        placement_t placement = {.function = function};
        uint32_t locals = function->n_locals;
        ir_local_t **moved = core_alloc_array(ir_local_t *, locals + 1);
        bool *entry = core_alloc_array(bool, locals + 1);
        uint32_t count = 0;
        ir_doms_t *doms = ir_doms_of(function);
        for (uint32_t l = 0; doms && l < locals; l++) {
            ir_local_t *local = function->locals[l];
            if (!local->address_taken
                || !local_escapes(summaries, function, local)) continue;
            entry[count] = needs_entry_storage(doms, function, local);
            moved[count++] = local;
        }
        if (doms) ir_doms_free(doms);
        bool ok = doms != nullptr;
        for (uint32_t m = 0; ok && m < count; m++)
            ok = move_local_to_storage(&placement, types, moved[m],
                                       entry[m]);
        core_free(moved);
        core_free(entry);
        if (!ok) return false;
        if (!annotate_allocations(context, summaries, function))
            return false;
    }
    return true;
}
