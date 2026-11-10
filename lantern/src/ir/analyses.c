#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/abcd.h"
#include "ir/liveness.h"
#include "ir/vn.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/list.h"
#include "th/typehash.h"

struct ir_func_analyses_t {
    uint64_t            fingerprint;
    uint64_t            epoch;
    uint64_t            checked;
    ir_doms_t          *doms;
    ir_pdg_t           *pdg;
    ir_places_t        *places;
    ir_value_numbers_t *numbers;
    ir_sites_t         *sites;
    ir_abcd_t          *abcd;
    ir_live_t          *live;
};

typedef ir_func_t *ir_func_ptr_t;
core_list_decl(ir_func_ptr_t);

static _Thread_local core_list_t(ir_func_ptr_t) scope_owned;
static _Thread_local size_t scope_depth;
static _Thread_local ir_summaries_t *bound_summaries;
static _Thread_local uint64_t summary_epoch = 1;
static _Thread_local size_t read_only_depth;
static _Thread_local uint64_t read_only_epoch;

void
ir_analysis_read_only_begin(void)
{
    if (!read_only_depth++) read_only_epoch++;
}

void
ir_analysis_read_only_end(void)
{
    if (read_only_depth) read_only_depth--;
}

ir_summaries_t *
ir_analysis_bind_summaries(ir_summaries_t *summaries)
{
    ir_summaries_t *previous = bound_summaries;
    if (summaries != previous) summary_epoch++;
    bound_summaries = summaries;
    return previous;
}

ir_summaries_t *
ir_analysis_summaries(void)
{
    return bound_summaries;
}

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
    while (scope_depth == 1 && scope_owned.len > scope)
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
mix_annots(uint64_t h, const ir_annots_t *annots, bool with)
{
    for (uint32_t i = 0; with && i < annots->n; i++) {
        if (annots->a[i].derived_from) continue;
        h = mix(h, core_xxh3_64bits_raw(&annots->a[i], sizeof(ir_annot_t)));
    }
    return h;
}

static uint64_t
mix_type(uint64_t h, th_type *type, core_dict_t *seen, uint32_t depth)
{
    h = mix(h, (uint64_t)(uintptr_t)type);
    if (!type || depth > 64 || !core_dict_add(seen, type, type)) return h;
    if (th_kind_of(type) == TH_VAR) {
        h = mix(h, th_var_revision(type));
        th_type *resolved = th_resolve(type);
        return resolved == type ? h
                                : mix_type(h, resolved, seen, depth + 1);
    }
    for (size_t c = 0; c < th_child_count(type); c++)
        h = mix_type(h, th_child(type, c), seen, depth + 1);
    return h;
}

static uint64_t
mix_value(uint64_t h, const ir_value_t *value, bool annots,
          core_dict_t *seen)
{
    if (!value) return mix(h, 0);
    h = mix(h, (uint64_t)(uintptr_t)value);
    h = mix_type(h, value->type, seen, 0);
    return mix_annots(h, &value->annots, annots);
}

static uint64_t
mix_outside_types(uint64_t h, const ir_instr_t *in, core_dict_t *seen)
{
    for (uint32_t o = 0; o < in->n_operands; o++) {
        const ir_value_t *value = ir_operand_value(&in->operands[o]);
        if (value && (value->kind == IR_VAL_GLOBAL
                      || value->kind == IR_VAL_FUNC))
            h = mix_type(h, value->type, seen, 0);
    }
    return h;
}

static uint64_t
fingerprint(const ir_func_t *f, bool annots)
{
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    uint64_t h = mix(0, f->n_blocks);
    h = mix_type(h, f->type, &seen, 0);
    h = mix_annots(h, &f->annots, annots);
    h = mix(h, f->n_params);
    for (uint32_t p = 0; p < f->n_params; p++)
        h = mix_value(h, f->params[p], annots, &seen);
    h = mix(h, f->n_locals);
    for (uint32_t l = 0; l < f->n_locals; l++) {
        const ir_local_t *local = f->locals[l];
        h = mix(h, (uint64_t)(uintptr_t)local);
        h = mix_type(h, local->type, &seen, 0);
        h = mix(h, local->address_taken);
        if (local->value) h = mix_annots(h, &local->value->annots, annots);
    }
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        const ir_block_t *block = f->blocks[b];
        h = mix(h, (uint64_t)(uintptr_t)block);
        h = mix(h, block->n_instrs);
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            h = mix(h, (uint64_t)(uintptr_t)in);
            h = mix(h, in->op);
            h = mix(h, in->dest_kind);
            h = mix_value(h, in->dest, annots, &seen);
            h = mix(h, (uint64_t)(uintptr_t)in->dest_local);
            h = mix(h, in->n_operands);
            if (in->n_operands)
                h = mix(h, core_xxh3_64bits_raw(in->operands,
                    in->n_operands * sizeof(ir_operand_t)));
            h = mix_outside_types(h, in, &seen);
            h = mix(h, in->n_mods);
            if (in->n_mods)
                h = mix(h, core_xxh3_64bits_raw(in->mods,
                    in->n_mods * sizeof(core_string_t)));
            h = mix(h, (uint64_t)(uintptr_t)in->const_bytes);
            h = mix(h, in->const_byte_len);
            h = mix_annots(h, &in->annots, annots);
        }
    }
    core_dict_free(&seen);
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

static void
release_summary_dependents(struct ir_func_analyses_t *slot)
{
    ir_abcd_free(slot->abcd);
    slot->abcd = nullptr;
    ir_value_numbers_free(slot->numbers);
    slot->numbers = nullptr;
    ir_pdg_free(slot->pdg);
    slot->pdg = nullptr;
    slot->epoch = summary_epoch;
}

static struct ir_func_analyses_t *
current_slot(ir_func_t *f)
{
    struct ir_func_analyses_t *slot = slot_of(f);
    uint64_t now = fingerprint(f, true);
    if (now == slot->fingerprint) {
        if (slot->epoch != summary_epoch) release_summary_dependents(slot);
        return slot;
    }
    release_summary_dependents(slot);
    ir_sites_free(slot->sites);
    slot->sites = nullptr;
    ir_places_free(slot->places);
    ir_live_free(slot->live);
    ir_doms_free(slot->doms);
    slot->places = nullptr;
    slot->live = nullptr;
    slot->doms = nullptr;
    slot->fingerprint = now;
    return slot;
}

static _Thread_local const ir_func_t *building;

static struct ir_func_analyses_t *
valid_slot(ir_func_t *f)
{
    if (building == f) return slot_of(f);
    if (!read_only_depth) return current_slot(f);
    struct ir_func_analyses_t *slot = slot_of(f);
    if (slot->checked != read_only_epoch) {
        slot = current_slot(f);
        slot->checked = read_only_epoch;
    }
    else if (slot->epoch != summary_epoch) release_summary_dependents(slot);
    return slot;
}

ir_doms_t *
ir_func_doms(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    struct ir_func_analyses_t *slot = valid_slot(f);
    if (!slot->doms) slot->doms = ir_doms_of(f);
    return slot->doms;
}

ir_pdg_t *
ir_func_control(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    struct ir_func_analyses_t *slot = valid_slot(f);
    if (!slot->doms) slot->doms = ir_doms_of(f);
    if (!slot->pdg && slot->doms) {
        const ir_func_t *outer = building;
        building = f;
        slot->pdg = ir_pdg_with_doms(f, slot->doms);
        building = outer;
    }
    return slot->pdg;
}

ir_pdg_t *
ir_func_pdg(ir_func_t *f)
{
    ir_pdg_t *pdg = ir_func_control(f);
    if (pdg) {
        const ir_func_t *outer = building;
        building = f;
        ir_pdg_complete(pdg);
        building = outer;
    }
    return pdg;
}

ir_places_t *
ir_func_places(ir_func_t *f)
{
    if (!f) return nullptr;
    struct ir_func_analyses_t *slot = valid_slot(f);
    if (!slot->places) {
        const ir_func_t *outer = building;
        building = f;
        slot->places = ir_places_build(f);
        building = outer;
    }
    return slot->places;
}

ir_value_numbers_t *
ir_func_value_numbers(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    struct ir_func_analyses_t *slot = valid_slot(f);
    if (!slot->numbers) {
        ir_pdg_t *pdg = ir_func_pdg(f);
        const ir_func_t *outer = building;
        building = f;
        slot->numbers = ir_value_numbers_build(f, pdg);
        building = outer;
    }
    return slot->numbers;
}

ir_sites_t *
ir_func_sites(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    struct ir_func_analyses_t *slot = valid_slot(f);
    if (!slot->sites) {
        const ir_func_t *outer = building;
        building = f;
        slot->sites = ir_sites_build(f);
        building = outer;
    }
    return slot->sites;
}

ir_abcd_t *
ir_func_abcd(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    struct ir_func_analyses_t *slot = valid_slot(f);
    if (!slot->abcd) {
        ir_func_value_numbers(f);
        ir_func_pdg(f);
        const ir_func_t *outer = building;
        building = f;
        slot->abcd = ir_abcd_build(f);
        building = outer;
    }
    return slot->abcd;
}

uint64_t
ir_func_shape(const ir_func_t *f)
{
    return fingerprint(f, false);
}

void
ir_func_release_analyses(ir_func_t *f)
{
    if (!f || !f->analyses) return;
    struct ir_func_analyses_t *slot = f->analyses;
    ir_abcd_free(slot->abcd);
    ir_sites_free(slot->sites);
    ir_value_numbers_free(slot->numbers);
    ir_places_free(slot->places);
    ir_pdg_free(slot->pdg);
    ir_live_free(slot->live);
    ir_doms_free(slot->doms);
    core_free(slot);
    f->analyses = nullptr;
}

ir_live_t *
ir_func_liveness(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    struct ir_func_analyses_t *slot = valid_slot(f);
    if (!slot->live) {
        const ir_func_t *outer = building;
        building = f;
        slot->live = ir_live_build(f);
        building = outer;
    }
    return slot->live;
}

void
ir_module_release_analyses(ir_module_t *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->n_funcs; i++)
        ir_func_release_analyses(m->funcs[i]);
}
