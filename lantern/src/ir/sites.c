#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/sites.h"
#include "lib/alloc.h"
#include "lib/list.h"

core_list_decl(ir_site_t);
typedef ir_block_t *site_block_ptr_t;
core_list_decl(site_block_ptr_t);

struct ir_sites_t {
    core_list_t(ir_site_t) sites;
};

static void
add_site(ir_sites_t *sites, ir_instr_t *in, uint32_t operand,
         ir_site_kind_t kind)
{
    core_list_push(sites->sites,
                   ((ir_site_t){.instr = in, .operand = operand, .kind = kind}));
}

static bool
traps(const ir_instr_t *in)
{
    switch (in->op) {
    case IR_OP_TRAP:
    case IR_OP_INDEX:
    case IR_OP_SEQUENCE_GET:
    case IR_OP_SEQUENCE_SET:
    case IR_OP_OPTION_GET:
        return true;
    default:
        return ir_instr_has_mod((ir_instr_t *)in, CORE_STRING_STATIC("trap"));
    }
}

static bool *
loop_members(ir_func_t *f, ir_doms_t *doms, ir_block_t *header,
             ir_block_t *latch)
{
    bool *in_loop = core_alloc_array(bool, f->n_blocks);
    core_list_t(site_block_ptr_t) work = core_list_new(site_block_ptr_t);
    in_loop[header->index] = true;
    if (!in_loop[latch->index]) {
        in_loop[latch->index] = true;
        core_list_push(work, latch);
    }
    while (work.len) {
        ir_block_t *block = work.data[--work.len];
        for (uint32_t p = 0; p < ir_pred_count(doms, block); p++) {
            ir_block_t *pred = ir_pred_at(doms, block, p);
            if (pred && ir_block_reachable(doms, pred) && !in_loop[pred->index]) {
                in_loop[pred->index] = true;
                core_list_push(work, pred);
            }
        }
    }
    core_list_free(work);
    return in_loop;
}

static void
mark_loop_bounds(ir_func_t *f, ir_doms_t *doms, bool *bound)
{
    for (uint32_t h = 0; h < f->n_blocks; h++) {
        ir_block_t *header = f->blocks[h];
        if (!ir_block_reachable(doms, header)) continue;
        for (uint32_t p = 0; p < ir_pred_count(doms, header); p++) {
            ir_block_t *latch = ir_pred_at(doms, header, p);
            if (!latch || !ir_dominates(doms, header, latch)) continue;
            bool *in_loop = loop_members(f, doms, header, latch);
            for (uint32_t b = 0; b < f->n_blocks; b++) {
                ir_block_t *block = f->blocks[b];
                ir_instr_t *t = in_loop[block->index]
                    ? ir_block_terminator(block) : nullptr;
                if (!t || bound[block->index]
                    || (t->op != IR_OP_BRANCH && t->op != IR_OP_SWITCH))
                    continue;
                for (uint32_t s = 0; s < ir_succ_count(t); s++) {
                    ir_block_t *succ = ir_succ_at(t, s);
                    if (succ && !in_loop[succ->index]) {
                        bound[block->index] = true;
                        break;
                    }
                }
            }
            core_free(in_loop);
        }
    }
}

ir_sites_t *
ir_sites_build(ir_func_t *f)
{
    ir_doms_t *doms = ir_func_doms(f);
    if (!doms) return nullptr;
    ir_sites_t *sites = core_alloc(ir_sites_t);
    sites->sites = core_list_new(ir_site_t);
    bool *bound = core_alloc_array(bool, f->n_blocks);
    mark_loop_bounds(f, doms, bound);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        if (!ir_block_reachable(doms, block)) continue;
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (traps(in)) add_site(sites, in, UINT32_MAX, IR_SITE_TRAP);
            switch (in->op) {
            case IR_OP_BRANCH:
            case IR_OP_SWITCH:
                add_site(sites, in, 0, bound[block->index]
                    ? IR_SITE_LOOP_BOUND : IR_SITE_BRANCH);
                break;
            case IR_OP_SELECT:
                add_site(sites, in, 0, IR_SITE_BRANCH);
                break;
            case IR_OP_LOAD:
            case IR_OP_STORE:
            case IR_OP_MEMSET:
                add_site(sites, in, 0, IR_SITE_ADDRESS);
                break;
            case IR_OP_MEMCPY:
            case IR_OP_MEMMOVE:
            case IR_OP_MEMCMP:
                add_site(sites, in, 0, IR_SITE_ADDRESS);
                add_site(sites, in, 1, IR_SITE_ADDRESS);
                break;
            case IR_OP_INDEX:
            case IR_OP_SEQUENCE_GET:
            case IR_OP_SEQUENCE_SET:
                add_site(sites, in, 1, IR_SITE_ADDRESS);
                break;
            case IR_OP_DIV:
            case IR_OP_REM:
                add_site(sites, in, 1, IR_SITE_DIVISOR);
                break;
            case IR_OP_SHL:
            case IR_OP_SHR:
            case IR_OP_ROL:
            case IR_OP_ROR:
                add_site(sites, in, 1, IR_SITE_SHIFT_AMOUNT);
                break;
            case IR_OP_BOX:
                add_site(sites, in, 0, IR_SITE_BOXED);
                break;
            case IR_OP_CALL:
            case IR_OP_CALLBACK_INVOKE:
                for (uint32_t o = 1; o < in->n_operands; o++)
                    add_site(sites, in, o, IR_SITE_CALL_ARGUMENT);
                if (in->dest_kind != IR_DEST_NONE)
                    add_site(sites, in, UINT32_MAX, IR_SITE_CALL_RESULT);
                break;
            case IR_OP_CALL_OUTPUT:
                add_site(sites, in, UINT32_MAX, IR_SITE_COPY_OUT);
                break;
            case IR_OP_RETURN:
                if (in->n_operands) add_site(sites, in, 0, IR_SITE_RETURNED);
                break;
            case IR_OP_FAIL:
                add_site(sites, in, in->n_operands ? 0 : UINT32_MAX,
                         IR_SITE_FAILURE);
                break;
            default:
                break;
            }
        }
    }
    core_free(bound);
    return sites;
}

void
ir_sites_free(ir_sites_t *sites)
{
    if (!sites) return;
    core_list_free(sites->sites);
    core_free(sites);
}

uint32_t
ir_sites_count(const ir_sites_t *sites)
{
    return sites ? (uint32_t)sites->sites.len : 0;
}

const ir_site_t *
ir_site_at(const ir_sites_t *sites, uint32_t index)
{
    return sites && index < sites->sites.len ? &sites->sites.data[index]
                                             : nullptr;
}

static bool
reaches_exit(ir_func_t *f, ir_instr_predicate_fn passes,
             ir_instr_predicate_fn diverges, void *context, bool failures)
{
    ir_doms_t  *doms  = ir_func_doms(f);
    ir_block_t *entry = ir_block_entry(f);
    if (!doms || !entry) return true;
    bool *seen = core_alloc_array(bool, f->n_blocks);
    core_list_t(site_block_ptr_t) work = core_list_new(site_block_ptr_t);
    seen[entry->index] = true;
    core_list_push(work, entry);
    bool found = false;
    while (!found && work.len) {
        ir_block_t *block = work.data[--work.len];
        bool        ended = false;
        for (uint32_t i = 0; !ended && i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if ((passes && passes(context, in))
                || (diverges && diverges(context, in))) {
                ended = true;
                break;
            }
            if (in->op == IR_OP_RETURN
                || (failures && in->op == IR_OP_FAIL)) {
                found = true;
                ended = true;
            }
            if (in->op == IR_OP_TRAP || in->op == IR_OP_UNREACHABLE)
                ended = true;
        }
        ir_instr_t *t = ended ? nullptr : ir_block_terminator(block);
        for (uint32_t s = 0; t && s < ir_succ_count(t); s++) {
            ir_block_t *succ = ir_succ_at(t, s);
            if (succ && !seen[succ->index]) {
                seen[succ->index] = true;
                core_list_push(work, succ);
            }
        }
    }
    core_list_free(work);
    core_free(seen);
    return found;
}

bool
ir_paths_can_return(ir_func_t *f, ir_instr_predicate_fn diverges,
                    void *context)
{
    return f && reaches_exit(f, nullptr, diverges, context, true);
}

bool
ir_paths_all_pass(ir_func_t *f, ir_instr_predicate_fn passes,
                  ir_instr_predicate_fn diverges, void *context)
{
    return f && passes && !reaches_exit(f, passes, diverges, context, true);
}
