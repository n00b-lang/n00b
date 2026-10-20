#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "lib/alloc.h"
#include "lib/hash.h"
#include "lib/list.h"

struct ir_func_analyses_t {
    uint64_t     fingerprint;
    ir_doms_t   *doms;
    ir_pdg_t    *pdg;
    ir_places_t *places;
    ir_memssa_t *memssa;
};

typedef ir_func_t *ir_func_ptr_t;
core_list_decl(ir_func_ptr_t);

static _Thread_local core_list_t(ir_func_ptr_t) scope_owned;
static _Thread_local size_t scope_depth;

size_t
ir_analysis_scope_begin(void)
{
    if (!scope_depth++ && !scope_owned.data)
        scope_owned = core_list_new(ir_func_ptr_t);
    return scope_owned.len;
}

void
ir_analysis_scope_end(size_t scope)
{
    while (scope_owned.len > scope)
        ir_func_release_analyses(scope_owned.data[--scope_owned.len]);
    if (scope_depth && !--scope_depth) core_list_free(scope_owned);
}

static uint64_t
mix(uint64_t state, uint64_t word)
{
    uint64_t z = state + word + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t
fingerprint(const ir_func_t *f)
{
    uint64_t h = mix(0, f->n_blocks);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        const ir_block_t *block = f->blocks[b];
        h = mix(h, (uint64_t)(uintptr_t)block);
        h = mix(h, block->n_instrs);
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            h = mix(h, (uint64_t)(uintptr_t)in);
            h = mix(h, in->op);
            h = mix(h, (uint64_t)(uintptr_t)in->dest);
            h = mix(h, (uint64_t)(uintptr_t)in->dest_local);
            h = mix(h, in->n_operands);
            if (in->n_operands)
                h = mix(h, core_xxh3_64bits_raw(in->operands,
                    in->n_operands * sizeof(ir_operand_t)));
        }
    }
    return h;
}

static void
release_slot(void *argument)
{
    ir_module_release_analyses(argument);
}

static struct ir_func_analyses_t *
slot_of(ir_func_t *f)
{
    if (f->analyses) return f->analyses;
    ir_module_t *m = f->owner;
    if (m && !m->analyses_hooked) {
        ir_ctx_on_free(m->ctx, release_slot, m);
        m->analyses_hooked = true;
    }
    struct ir_func_analyses_t *slot = core_alloc(struct ir_func_analyses_t);
    f->analyses = slot;
    if (scope_depth) core_list_push(scope_owned, f);
    return slot;
}

static struct ir_func_analyses_t *
current_slot(ir_func_t *f)
{
    struct ir_func_analyses_t *slot = slot_of(f);
    uint64_t now = fingerprint(f);
    if (now == slot->fingerprint) return slot;
    ir_memssa_free(slot->memssa);
    slot->memssa = nullptr;
    ir_places_free(slot->places);
    ir_pdg_free(slot->pdg);
    ir_doms_free(slot->doms);
    slot->places = nullptr;
    slot->pdg = nullptr;
    slot->doms = nullptr;
    slot->fingerprint = now;
    return slot;
}

ir_doms_t *
ir_func_doms(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    struct ir_func_analyses_t *slot = current_slot(f);
    if (!slot->doms) slot->doms = ir_doms_of(f);
    return slot->doms;
}

ir_pdg_t *
ir_func_pdg(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    struct ir_func_analyses_t *slot = current_slot(f);
    if (!slot->doms) slot->doms = ir_doms_of(f);
    if (!slot->pdg && slot->doms) slot->pdg = ir_pdg_with_doms(f, slot->doms);
    return slot->pdg;
}

ir_places_t *
ir_func_places(ir_func_t *f)
{
    if (!f) return nullptr;
    struct ir_func_analyses_t *slot = current_slot(f);
    if (!slot->places) slot->places = ir_places_build(f);
    return slot->places;
}

ir_memssa_t *
ir_func_memssa(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    struct ir_func_analyses_t *slot = current_slot(f);
    if (!slot->memssa) slot->memssa = ir_memssa_build(f);
    return slot->memssa;
}

void
ir_func_release_analyses(ir_func_t *f)
{
    if (!f || !f->analyses) return;
    struct ir_func_analyses_t *slot = f->analyses;
    ir_memssa_free(slot->memssa);
    ir_places_free(slot->places);
    ir_pdg_free(slot->pdg);
    ir_doms_free(slot->doms);
    core_free(slot);
    f->analyses = nullptr;
}

void
ir_module_release_analyses(ir_module_t *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->n_funcs; i++)
        ir_func_release_analyses(m->funcs[i]);
}
