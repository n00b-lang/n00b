#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/sites.h"
#include "lib/alloc.h"
#include "lib/list.h"

core_list_decl(ir_site_t);
core_list_decl(ir_dead_site_t);
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

static uint32_t
instr_sites(const ir_instr_t *in, ir_site_t *out, uint32_t capacity)
{
    uint32_t count = 0;
#define SITE(operand_, kind_)                                               \
    do {                                                                    \
        if (count < capacity)                                               \
            out[count++] = (ir_site_t){.instr = (ir_instr_t *)in,           \
                                       .operand = (operand_),               \
                                       .kind = (kind_)};                    \
    } while (0)
    switch (in->op) {
    case IR_OP_BRANCH:
    case IR_OP_SWITCH:
    case IR_OP_SELECT:
        SITE(0, IR_SITE_BRANCH);
        break;
    case IR_OP_LOAD:
    case IR_OP_STORE:
    case IR_OP_MEMSET:
    case IR_OP_ATOMIC_RMW:
    case IR_OP_ATOMIC_CAS:
        SITE(0, IR_SITE_ADDRESS);
        break;
    case IR_OP_MEMCPY:
    case IR_OP_MEMMOVE:
    case IR_OP_MEMCMP:
        SITE(0, IR_SITE_ADDRESS);
        SITE(1, IR_SITE_ADDRESS);
        break;
    case IR_OP_INDEX:
    case IR_OP_SEQUENCE_GET:
    case IR_OP_SEQUENCE_SET:
        SITE(1, IR_SITE_ADDRESS);
        break;
    case IR_OP_DIV:
    case IR_OP_REM:
        SITE(1, IR_SITE_DIVISOR);
        break;
    case IR_OP_SHL:
    case IR_OP_SHR:
        SITE(1, IR_SITE_SHIFT_AMOUNT);
        break;
    case IR_OP_BOX:
    case IR_OP_BOX_RAW:
    case IR_OP_BOX_FROM_RAW:
        SITE(0, IR_SITE_BOXED);
        break;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE:
        if (in->dest_kind != IR_DEST_NONE)
            SITE(UINT32_MAX, IR_SITE_CALL_RESULT);
        break;
    case IR_OP_CALL_OUTPUT:
        SITE(UINT32_MAX, IR_SITE_COPY_OUT);
        break;
    case IR_OP_RETURN:
        if (in->n_operands) SITE(0, IR_SITE_RETURNED);
        break;
    case IR_OP_FAIL:
        SITE(in->n_operands ? 0 : UINT32_MAX, IR_SITE_FAILURE);
        break;
    default:
        break;
    }
#undef SITE
    return count;
}

bool
ir_instr_site(const ir_instr_t *in, ir_site_kind_t kind, uint32_t *operand)
{
    ir_site_t found[4];
    uint32_t  count = in ? instr_sites(in, found, 4) : 0;
    for (uint32_t s = 0; s < count; s++)
        if (found[s].kind == kind) {
            if (operand) *operand = found[s].operand;
            return true;
        }
    return false;
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
            ir_site_t found[4];
            uint32_t  count = instr_sites(in, found, 4);
            for (uint32_t s = 0; s < count; s++) {
                ir_site_kind_t kind = found[s].kind;
                if (kind == IR_SITE_BRANCH && in->op != IR_OP_SELECT
                    && bound[block->index])
                    kind = IR_SITE_LOOP_BOUND;
                add_site(sites, in, found[s].operand, kind);
            }
            if (in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                for (uint32_t o = 1; o < in->n_operands; o++)
                    add_site(sites, in, o, IR_SITE_CALL_ARGUMENT);
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

ir_instr_t *
ir_call_outcome_branch(const ir_instr_t *terminator, bool *true_is_success)
{
    if (!terminator || terminator->op != IR_OP_BRANCH
        || terminator->n_operands < 3) return nullptr;
    ir_value_t *condition  = ir_operand_value(&terminator->operands[0]);
    ir_instr_t *comparison = condition ? condition->def : nullptr;
    if (!comparison || (comparison->op != IR_OP_EQ && comparison->op != IR_OP_NE)
        || comparison->n_operands != 2) return nullptr;
    for (uint32_t side = 0; side < 2; side++) {
        ir_value_t *tag_value = ir_operand_value(&comparison->operands[side]);
        ir_value_t *literal_value =
            ir_operand_value(&comparison->operands[1 - side]);
        ir_instr_t *tag     = tag_value ? tag_value->def : nullptr;
        ir_instr_t *literal = literal_value ? literal_value->def : nullptr;
        ir_value_t *variant = tag && tag->op == IR_OP_VARIANT_TAG
            && tag->n_operands == 1 ? ir_operand_value(&tag->operands[0])
                                    : nullptr;
        if (!variant || !variant->def || variant->def->op != IR_OP_CALL
            || !literal || literal->op != IR_OP_CONST
            || literal->n_operands != 1
            || literal->operands[0].kind != IR_OPD_CONST
            || literal->operands[0].lit.i.neg
            || literal->operands[0].lit.i.mag
                != th_arm_tag(CORE_STRING_STATIC("ok")))
            continue;
        if (true_is_success) *true_is_success = comparison->op == IR_OP_EQ;
        return variant->def;
    }
    return nullptr;
}

uint8_t
ir_path_apply(uint8_t state, uint8_t effect)
{
    uint8_t result = 0;
    if (state & IR_PATH_PASSED) result |= effect ? IR_PATH_PASSED : 0;
    if (state & IR_PATH_UNPASSED) result |= effect;
    return result;
}

static uint8_t
step_of(ir_path_step_fn step, void *context, const ir_instr_t *in,
        ir_path_outcome_t outcome)
{
    return step ? step(context, in, outcome) : IR_PATH_UNPASSED;
}

static bool
merge_state(uint8_t *into, uint8_t state)
{
    uint8_t merged  = *into | state;
    bool    changed = merged != *into;
    *into = merged;
    return changed;
}

static uint8_t *paths_solve(ir_func_t *f, uint8_t entry, ir_path_step_fn step,
                            void *context, ir_path_exits_t *exits);

bool
ir_paths_flow(ir_func_t *f, uint8_t entry, ir_path_step_fn step,
              void *context, ir_path_exits_t *exits)
{
    uint8_t *incoming = paths_solve(f, entry, step, context, exits);
    bool solved = incoming != nullptr;
    core_free(incoming);
    return solved;
}

bool
ir_paths_reach(ir_func_t *f, ir_path_step_fn step, void *context,
               const ir_instr_t *at)
{
    ir_path_exits_t exits;
    uint8_t *incoming = paths_solve(f, IR_PATH_UNPASSED, step, context,
                                    &exits);
    if (!incoming) return true;
    uint8_t state = incoming[at->block->index];
    for (uint32_t i = 0; state && i < at->index; i++)
        state = ir_path_apply(state, step_of(step, context,
                                             at->block->instrs[i],
                                             IR_PATH_ANY));
    core_free(incoming);
    return state != 0;
}

static uint8_t *
paths_solve(ir_func_t *f, uint8_t entry, ir_path_step_fn step, void *context,
            ir_path_exits_t *exits)
{
    *exits = (ir_path_exits_t){0};
    ir_doms_t  *doms  = f ? ir_func_doms(f) : nullptr;
    ir_block_t *first = f ? ir_block_entry(f) : nullptr;
    if (!doms || !first) return nullptr;
    uint8_t *incoming = core_alloc_array(uint8_t, f->n_blocks);
    incoming[first->index] = entry;
    bool changed;
    do {
        changed = false;
        for (uint32_t b = 0; b < f->n_blocks; b++) {
            ir_block_t *block = f->blocks[b];
            if (!ir_block_reachable(doms, block)) continue;
            uint8_t state = incoming[block->index];
            if (!state) continue;
            ir_instr_t *terminator      = ir_block_terminator(block);
            bool        true_is_success = false;
            ir_instr_t *outcome = ir_call_outcome_branch(terminator,
                                                         &true_is_success);
            bool    deferred = false;
            uint8_t before   = 0;
            uint8_t rest     = IR_PATH_UNPASSED;
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                ir_instr_t *in = block->instrs[i];
                if (in == terminator) continue;
                if (in == outcome) {
                    before   = state;
                    deferred = true;
                    continue;
                }
                uint8_t effect = step_of(step, context, in, IR_PATH_ANY);
                state = ir_path_apply(state, effect);
                if (deferred) rest = ir_path_apply(rest, effect);
            }
            if (terminator && terminator->op == IR_OP_RETURN) {
                exits->returned |= state;
                continue;
            }
            if (terminator && terminator->op == IR_OP_FAIL) {
                exits->failed |= state;
                continue;
            }
            ir_block_t *true_block = deferred
                ? ir_succ_of(&terminator->operands[1]) : nullptr;
            for (uint32_t s = 0; terminator && s < ir_succ_count(terminator);
                 s++) {
                ir_block_t *succ = ir_succ_at(terminator, s);
                if (!succ) continue;
                uint8_t edge = state;
                if (deferred) {
                    bool success = (succ == true_block) == true_is_success;
                    edge = ir_path_apply(ir_path_apply(before,
                        step_of(step, context, outcome,
                                success ? IR_PATH_SUCCESS : IR_PATH_FAILURE)),
                        rest);
                }
                changed |= merge_state(&incoming[succ->index], edge);
            }
        }
    } while (changed);
    return incoming;
}

struct ir_dead_code_t {
    ir_func_t                   *f;
    bool                        *live;
    bool                        *covered;
    int64_t                     *cut;
    core_list_t(ir_dead_site_t)  sites;
    ir_const_varies_fn           varies;
    void                        *context;
};

static bool
user_instruction(const ir_instr_t *in)
{
    return in->span.start_line && in->span.start_byte && !in->span.implied
        && in->op != IR_OP_JUMP && in->op != IR_OP_RETURN
        && in->op != IR_OP_BRANCH && in->op != IR_OP_TRAP
        && in->op != IR_OP_UNREACHABLE;
}

static bool
constant_condition(const ir_dead_code_t *dead, const ir_instr_t *terminator,
                   bool *truth)
{
    if (!terminator || terminator->op != IR_OP_BRANCH
        || terminator->n_operands != 3) return false;
    const ir_value_t *condition = ir_operand_value(&terminator->operands[0]);
    const ir_instr_t *def = condition ? condition->def : nullptr;
    if (!def || def->op != IR_OP_CONST || def->n_operands != 1
        || def->operands[0].kind != IR_OPD_CONST
        || def->operands[0].lit.is_float
        || (dead->varies && dead->varies(dead->context, def))) return false;
    *truth = def->operands[0].lit.i.mag != 0;
    return true;
}

static const ir_instr_t *
earlier_site(const ir_instr_t *found, const ir_instr_t *in,
             const ir_instr_t *cause)
{
    if (!user_instruction(in)
        || (cause && (in->span.start_line < cause->span.end_line
            || (in->span.start_line == cause->span.end_line
                && in->span.start_col < cause->span.end_col))))
        return found;
    return !found || in->span.start_line < found->span.start_line
        || (in->span.start_line == found->span.start_line
            && in->span.start_col < found->span.start_col) ? in : found;
}

static const ir_instr_t *
dead_region_site(ir_dead_code_t *dead, ir_block_t *start, uint32_t after,
                 const ir_instr_t *cause)
{
    const ir_instr_t *found = nullptr;
    for (uint32_t i = after; i < start->n_instrs; i++)
        found = earlier_site(found, start->instrs[i], cause);
    core_list_t(site_block_ptr_t) work = core_list_new(site_block_ptr_t);
    core_list_push(work, start);
    for (size_t at = 0; at < work.len; at++) {
        ir_instr_t *terminator = ir_block_terminator(work.data[at]);
        for (uint32_t s = 0; terminator && s < ir_succ_count(terminator);
             s++) {
            ir_block_t *next = ir_succ_at(terminator, s);
            if (!next || dead->live[next->index]
                || dead->covered[next->index]) continue;
            dead->covered[next->index] = true;
            core_list_push(work, next);
            for (uint32_t i = 0; i < next->n_instrs; i++)
                found = earlier_site(found, next->instrs[i], cause);
        }
    }
    core_list_free(work);
    return found;
}

static void
add_dead_site(ir_dead_code_t *dead, const ir_instr_t *site,
              const ir_instr_t *cause, ir_dead_cause_t kind)
{
    if (site)
        core_list_push(dead->sites, ((ir_dead_site_t){
            .site = site, .cause = cause, .kind = kind}));
}

ir_dead_code_t *
ir_dead_code_build(ir_func_t *f, ir_call_diverges_fn diverges,
                   ir_const_varies_fn varies, void *context)
{
    if (!f || !f->n_blocks) return nullptr;
    ir_dead_code_t *dead = core_alloc(ir_dead_code_t);
    dead->f       = f;
    dead->live    = core_alloc_array(bool, f->n_blocks);
    dead->covered = core_alloc_array(bool, f->n_blocks);
    dead->cut     = core_alloc_array(int64_t, f->n_blocks);
    dead->sites   = core_list_new(ir_dead_site_t);
    dead->varies  = varies;
    dead->context = context;
    for (uint32_t b = 0; b < f->n_blocks; b++) dead->cut[b] = -1;
    ir_block_t *entry = ir_block_entry(f);
    core_list_t(site_block_ptr_t) work = core_list_new(site_block_ptr_t);
    dead->live[entry->index] = true;
    core_list_push(work, entry);
    while (work.len) {
        ir_block_t *block = work.data[--work.len];
        for (uint32_t i = 0; dead->cut[block->index] < 0 && i < block->n_instrs;
             i++) {
            const ir_instr_t *in = block->instrs[i];
            if ((in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                && diverges && diverges(context, in))
                dead->cut[block->index] = i;
        }
        if (dead->cut[block->index] >= 0) continue;
        ir_instr_t *terminator = ir_block_terminator(block);
        bool truth = false;
        bool constant = constant_condition(dead, terminator, &truth);
        for (uint32_t s = 0; terminator && s < ir_succ_count(terminator);
             s++) {
            ir_block_t *next = ir_succ_at(terminator, s);
            if (constant && next
                && next != ir_succ_of(&terminator->operands[truth ? 1 : 2]))
                continue;
            if (!next || dead->live[next->index]) continue;
            dead->live[next->index] = true;
            core_list_push(work, next);
        }
    }
    core_list_free(work);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        if (!dead->live[b]) continue;
        if (dead->cut[b] >= 0) {
            add_dead_site(dead, dead_region_site(dead, block,
                              (uint32_t)dead->cut[b] + 1,
                              block->instrs[dead->cut[b]]),
                          block->instrs[dead->cut[b]],
                          IR_DEAD_AFTER_DIVERGING_CALL);
            continue;
        }
        ir_instr_t *terminator = ir_block_terminator(block);
        bool truth = false;
        if (!constant_condition(dead, terminator, &truth)) continue;
        ir_block_t *untaken = ir_succ_of(&terminator->operands[truth ? 2 : 1]);
        if (!untaken || dead->live[untaken->index]
            || dead->covered[untaken->index]) continue;
        dead->covered[untaken->index] = true;
        add_dead_site(dead, dead_region_site(dead, untaken, 0, nullptr),
                      terminator,
                      IR_DEAD_CONSTANT_CONDITION);
    }
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        if (dead->live[b] || dead->covered[b]) continue;
        dead->covered[b] = true;
        add_dead_site(dead, dead_region_site(dead, f->blocks[b], 0, nullptr),
                      nullptr,
                      IR_DEAD_UNREACHED);
    }
    return dead;
}

void
ir_dead_code_free(ir_dead_code_t *dead)
{
    if (!dead) return;
    core_free(dead->live);
    core_free(dead->covered);
    core_free(dead->cut);
    core_list_free(dead->sites);
    core_free(dead);
}

bool
ir_dead_code_instr(const ir_dead_code_t *dead, const ir_instr_t *in)
{
    if (!dead || !in || !in->block || in->block->owner != dead->f)
        return false;
    uint32_t b = in->block->index;
    return !dead->live[b]
        || (dead->cut[b] >= 0 && (int64_t)in->index > dead->cut[b]);
}

uint32_t
ir_dead_code_site_count(const ir_dead_code_t *dead)
{
    return dead ? (uint32_t)dead->sites.len : 0;
}

const ir_dead_site_t *
ir_dead_code_site_at(const ir_dead_code_t *dead, uint32_t index)
{
    return dead && index < dead->sites.len ? &dead->sites.data[index]
                                           : nullptr;
}
