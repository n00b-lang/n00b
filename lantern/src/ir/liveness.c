#include "ir/liveness.h"

#include <string.h>

#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/operands.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/list.h"

typedef struct {
    bool *live_in;
    bool *live_out;
} live_record_t;

typedef const ir_instr_t *live_copy_t;
core_list_decl(live_copy_t);

typedef struct {
    const ir_instr_t *user;
    uint32_t          operand;
} live_use_t;

typedef struct {
    ir_live_point_fn point;
    void            *context;
    size_t           count;
    ir_value_t     **values;
    bool            *across;
} across_memo_t;
core_list_decl(across_memo_t);

struct ir_live_t {
    ir_func_t   *f;
    uint32_t     count;
    bool         cfg_valid;
    ir_doms_t   *doms;
    int8_t      *cycle;
    core_dict_t  records;
    bool         uses_built;
    core_dict_t  use_index;
    uint32_t    *use_start;
    live_use_t  *uses;
    core_dict_t  copies;
    core_list_t(across_memo_t) across;
};


static bool
instruction_belongs_to(const ir_func_t *f, const ir_instr_t *in)
{
    return in && ir_block_belongs_to(f, in->block)
        && in->index < in->block->n_instrs
        && in->block->instrs[in->index] == in;
}

static bool
cfg_verified(const ir_func_t *f)
{
    ir_block_t *entry = f->n_blocks ? ir_block_entry((ir_func_t *)f)
                                    : nullptr;
    if (!entry || !ir_block_belongs_to(f, entry)) return false;
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        if (!ir_block_belongs_to(f, block) || !block->n_instrs) return false;
        uint32_t terminators = 0;
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            if (!instruction_belongs_to(f, in)) return false;
            bool is_terminator = ir_opinfo_is_terminator(
                ir_module_op_info(f->owner, in->op));
            terminators += is_terminator;
            if (is_terminator && i + 1 != block->n_instrs) return false;
        }
        ir_instr_t *terminator = ir_block_terminator(block);
        if (terminators != 1 || !terminator) return false;
        for (uint32_t s = 0; s < ir_succ_count(terminator); s++)
            if (!ir_block_belongs_to(f, ir_succ_at(terminator, s)))
                return false;
    }
    return true;
}

ir_live_t *
ir_live_build(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    ir_live_t *live = core_alloc(ir_live_t);
    live->f = f;
    live->count = f->n_blocks;
    live->cfg_valid = cfg_verified(f);
    core_dict_init(&live->records, core_hash_word, nullptr);
    core_dict_init(&live->use_index, core_hash_word, nullptr);
    core_dict_init(&live->copies, core_hash_word, nullptr);
    live->doms = live->cfg_valid ? ir_func_doms(f) : nullptr;
    live->cfg_valid = live->doms != nullptr;
    return live;
}

static bool
reachable(const ir_live_t *live, const ir_block_t *block)
{
    return ir_block_reachable(live->doms, (ir_block_t *)block);
}

uint32_t
ir_live_pred_count(const ir_live_t *live, const ir_block_t *block)
{
    return live && live->cfg_valid
        ? ir_pred_count(live->doms, (ir_block_t *)block) : 0;
}

ir_block_t *
ir_live_pred_at(const ir_live_t *live, const ir_block_t *block, uint32_t i)
{
    return ir_pred_at(live->doms, (ir_block_t *)block, i);
}

bool
ir_live_valid(const ir_live_t *live)
{
    return live && live->cfg_valid;
}

void
ir_live_free(ir_live_t *live)
{
    if (!live) return;
    core_dict_foreach(&live->records, item) {
        live_record_t *record = item->value;
        core_free(record->live_in);
        core_free(record->live_out);
        core_free(record);
    }
    core_dict_free(&live->records);
    core_dict_free(&live->use_index);
    core_dict_foreach(&live->copies, item) {
        core_list_t(live_copy_t) *list = item->value;
        core_list_free(*list);
        core_free(list);
    }
    core_dict_free(&live->copies);
    for (size_t i = 0; i < live->across.len; i++) {
        core_free(live->across.data[i].values);
        core_free(live->across.data[i].across);
    }
    core_list_free(live->across);
    core_free(live->use_start);
    core_free(live->uses);
    core_free(live->cycle);
    core_free(live);
}


static bool
kills(const ir_func_t *f, const ir_instr_t *in, const ir_value_t *value)
{
    if (in->dest == value) return true;
    return value->kind == IR_VAL_LOCAL && in->op == IR_OP_COPY
        && in->dest_local && ir_local_value((ir_func_t *)f, in->dest_local)
            == value;
}

static const void *
use_key(const ir_value_t *value)
{
    return value && value->kind == IR_VAL_GLOBAL && value->global
        ? (const void *)ir_global_identity(value->global) : value;
}

static bool
uses(const ir_instr_t *in, const ir_value_t *value, bool precise)
{
    if (precise && in->op == IR_OP_PHI) return false;
    const void *key = use_key(value);
    for (uint32_t o = 0; o < in->n_operands; o++)
        if (use_key(precise ? ir_operand_used_value(in, o)
                            : ir_operand_value(&in->operands[o])) == key)
            return true;
    return false;
}

static void
mark_live_out(live_record_t *record, const bool *kill, ir_block_t *block,
              ir_block_t **work, size_t *depth)
{
    record->live_out[block->index] = true;
    if (kill[block->index] || record->live_in[block->index]) return;
    record->live_in[block->index] = true;
    work[(*depth)++] = block;
}

static void build_uses(ir_live_t *live);
static bool is_edge(const ir_block_t *from, const ir_block_t *to);

static void
note_kill(uint32_t *first_kill, bool *kill, const ir_instr_t *in)
{
    uint32_t block = in->block->index;
    kill[block] = true;
    if (in->index < first_kill[block]) first_kill[block] = in->index;
}

static live_record_t *
record_of(ir_live_t *live, const ir_value_t *value, bool precise)
{
    const void *key = (const void *)((uintptr_t)use_key(value)
                                     | (uintptr_t)precise);
    bool found = false;
    live_record_t *record = core_dict_get(&live->records, key, &found);
    if (found) return record;
    if (!live->uses_built) build_uses(live);
    ir_func_t *f = live->f;
    uint32_t count = live->count;
    record = core_alloc(live_record_t);
    record->live_in = core_alloc_array(bool, count);
    record->live_out = core_alloc_array(bool, count);
    bool *kill = core_alloc_array(bool, count);
    uint32_t *first_kill = core_alloc_array(uint32_t, count);
    for (uint32_t b = 0; b < count; b++) first_kill[b] = UINT32_MAX;
    if (precise && value->kind == IR_VAL_INSTR && value->def
        && value->def->dest == value
        && instruction_belongs_to(f, value->def))
        note_kill(first_kill, kill, value->def);
    core_list_t(live_copy_t) *copies = precise
        && value->kind == IR_VAL_LOCAL
        ? core_dict_get(&live->copies, (void *)value, &found) : nullptr;
    for (size_t c = 0; copies && found && c < copies->len; c++)
        note_kill(first_kill, kill, copies->data[c]);
    ir_block_t **work = core_alloc_array(ir_block_t *, count);
    size_t depth = 0;
    uintptr_t slot = (uintptr_t)core_dict_get(&live->use_index,
                                              (void *)use_key(value), &found);
    for (uint32_t u = found ? live->use_start[slot] : 0;
         found && u < live->use_start[slot + 1]; u++) {
        const ir_instr_t *user = live->uses[u].user;
        const ir_operand_t *operand = &user->operands[live->uses[u].operand];
        uint32_t b = user->block->index;
        if (precise && user->op == IR_OP_PHI) {
            if (operand->kind == IR_OPD_PHI_ARM
                && is_edge(operand->phi.block, user->block))
                mark_live_out(record, kill, operand->phi.block, work,
                              &depth);
            continue;
        }
        if (!precise && operand->kind == IR_OPD_ARG) continue;
        if (user->index > first_kill[b] || record->live_in[b]) continue;
        record->live_in[b] = true;
        work[depth++] = user->block;
    }
    while (depth) {
        ir_block_t *block = work[--depth];
        for (uint32_t p = 0; p < ir_pred_count(live->doms, block); p++)
            mark_live_out(record, kill, ir_pred_at(live->doms, block, p),
                          work, &depth);
    }
    core_free(kill);
    core_free(first_kill);
    core_free(work);
    core_dict_put(&live->records, key, record);
    return record;
}

static bool
reaches(ir_live_t *live, const ir_value_t *value, const ir_instr_t *point,
        bool precise)
{
    const ir_block_t *block = point->block;
    for (uint32_t i = point->index + 1; i < block->n_instrs; i++) {
        const ir_instr_t *in = block->instrs[i];
        if (uses(in, value, precise)) return true;
        if (precise && kills(live->f, in, value)) return false;
    }
    return record_of(live, value, precise)->live_out[block->index];
}

static bool
reaches_after(ir_live_t *live, const ir_value_t *value,
              const ir_instr_t *point)
{
    if (!live || !live->cfg_valid || !value
        || !instruction_belongs_to(live->f, point)) return true;
    return reaches(live, value, point, true);
}

static bool
value_trackable(const ir_live_t *live, const ir_value_t *value)
{
    if (value && value->kind == IR_VAL_GLOBAL) return value->global;
    if (!value || value->owner != live->f) return false;
    if (value->kind == IR_VAL_PARAM || value->kind == IR_VAL_LOCAL)
        return true;
    return value->kind == IR_VAL_INSTR
        && instruction_belongs_to(live->f, value->def)
        && value->def->dest == value;
}

static bool
def_dominates(ir_live_t *live, const ir_value_t *value,
              const ir_block_t *block)
{
    if (value->kind != IR_VAL_INSTR) return true;
    ir_block_t *def_block = value->def->block;
    if (!reachable(live, def_block)) return false;
    return def_block == block
        || !reachable(live, block)
        || ir_dominates(live->doms, def_block, (ir_block_t *)block);
}

ir_liveness_t
ir_live_after(ir_live_t *live, const ir_value_t *value,
              const ir_instr_t *point)
{
    if (!live || !live->cfg_valid || !value_trackable(live, value)
        || !instruction_belongs_to(live->f, point)
        || !reachable(live, point->block)
        || !def_dominates(live, value, point->block)
        || (value->kind == IR_VAL_INSTR && value->def->block == point->block
            && value->def->index > point->index))
        return IR_LIVENESS_UNKNOWN;
    return reaches(live, value, point, false) ? IR_LIVENESS_LIVE
                                              : IR_LIVENESS_DEAD;
}

void
ir_live_after_each(ir_live_t *live, const ir_value_t *value,
                   const ir_block_t *block, ir_liveness_t *after)
{
    uint32_t count = block->n_instrs;
    bool known = live && live->cfg_valid && value_trackable(live, value)
        && ir_block_belongs_to(live->f, block) && reachable(live, block)
        && def_dominates(live, value, block);
    bool current = known
        && record_of(live, value, false)->live_out[block->index];
    uint32_t defined = known && value->kind == IR_VAL_INSTR
        && value->def->block == block ? value->def->index : 0;
    for (uint32_t i = count; i-- > 0;) {
        after[i] = !known || i < defined ? IR_LIVENESS_UNKNOWN
            : current ? IR_LIVENESS_LIVE : IR_LIVENESS_DEAD;
        current = current || uses(block->instrs[i], value, false);
    }
}

static bool
is_edge(const ir_block_t *from, const ir_block_t *to)
{
    ir_instr_t *terminator = ir_block_terminator((ir_block_t *)from);
    for (uint32_t s = 0; terminator && s < ir_succ_count(terminator); s++)
        if (ir_succ_at(terminator, s) == to) return true;
    return false;
}

ir_liveness_t
ir_live_on_edge(ir_live_t *live, const ir_value_t *value,
                const ir_block_t *from, const ir_block_t *to)
{
    if (!live || !live->cfg_valid || !value_trackable(live, value)
        || !ir_block_belongs_to(live->f, from) || !ir_block_belongs_to(live->f, to)
        || !is_edge(from, to) || !reachable(live, from))
        return IR_LIVENESS_UNKNOWN;
    if (value->kind == IR_VAL_INSTR) {
        const ir_block_t *def_block = value->def->block;
        if (def_block == from) {
            if (value->def->index
                >= ir_block_terminator((ir_block_t *)from)->index)
                return IR_LIVENESS_UNKNOWN;
        } else if (def_block == to || !def_dominates(live, value, from))
            return IR_LIVENESS_UNKNOWN;
    }
    return record_of(live, value, false)->live_in[to->index]
        ? IR_LIVENESS_LIVE : IR_LIVENESS_DEAD;
}

bool
ir_live_set_after_each(ir_live_t *live, ir_value_t *const *values,
                       size_t count, const ir_block_t *block, bool *after)
{
    if (!live || !live->cfg_valid || !ir_block_belongs_to(live->f, block))
        return false;
    bool *current = core_alloc_array(bool, count ? count : 1);
    size_t alive = 0;
    for (size_t v = 0; v < count; v++) {
        current[v] = record_of(live, values[v], true)->live_out[block->index];
        alive += current[v];
    }
    for (uint32_t i = block->n_instrs; i-- > 0;) {
        const ir_instr_t *in = block->instrs[i];
        after[i] = alive != 0;
        for (size_t v = 0; v < count; v++) {
            bool now = current[v];
            if (kills(live->f, in, values[v])) now = false;
            if (uses(in, values[v], true)) now = true;
            alive += (size_t)now - (size_t)current[v];
            current[v] = now;
        }
    }
    core_free(current);
    return true;
}

static bool
phi_edge_uses(const ir_block_t *from, const ir_block_t *to,
              const ir_value_t *value)
{
    for (uint32_t i = 0; i < to->n_instrs && to->instrs[i]->op == IR_OP_PHI;
         i++) {
        const ir_instr_t *phi = to->instrs[i];
        for (uint32_t o = 0; o < phi->n_operands; o++) {
            const ir_operand_t *operand = &phi->operands[o];
            if (operand->kind == IR_OPD_PHI_ARM && operand->phi.block == from
                && operand->phi.value == value) return true;
        }
    }
    return false;
}

bool
ir_live_set_on_edge(ir_live_t *live, ir_value_t *const *values, size_t count,
                    const ir_block_t *from, const ir_block_t *to)
{
    if (!live || !live->cfg_valid) return true;
    for (size_t v = 0; v < count; v++)
        if (phi_edge_uses(from, to, values[v])
            || record_of(live, values[v], true)->live_in[to->index])
            return true;
    return false;
}

typedef struct {
    ir_live_t   *live;
    core_dict_t  slots;
    size_t       words;
    uint64_t    *use;
    uint64_t    *def;
    uint64_t    *in;
    uint64_t    *out;
} across_t;

static bool
across_slot(across_t *a, const ir_value_t *value, size_t *slot)
{
    bool found = false;
    void *stored = value ? core_dict_get(&a->slots, (void *)value, &found)
                         : nullptr;
    if (found) *slot = (size_t)(uintptr_t)stored - 1;
    return found;
}

static void
across_set(uint64_t *bits, size_t slot)
{
    bits[slot / 64] |= (uint64_t)1 << (slot % 64);
}

static void
across_clear(uint64_t *bits, size_t slot)
{
    bits[slot / 64] &= ~((uint64_t)1 << (slot % 64));
}

static void
across_local_sets(across_t *a)
{
    ir_func_t *f = a->live->f;
    for (uint32_t b = 0; b < a->live->count; b++) {
        ir_block_t *block = f->blocks[b];
        uint64_t *use = a->use + (size_t)b * a->words;
        uint64_t *def = a->def + (size_t)b * a->words;
        for (uint32_t i = block->n_instrs; i-- > 0;) {
            const ir_instr_t *in = block->instrs[i];
            size_t slot = 0;
            if (across_slot(a, in->dest, &slot)) {
                across_set(def, slot);
                across_clear(use, slot);
            }
            if (in->op == IR_OP_PHI) continue;
            for (uint32_t o = 0; o < in->n_operands; o++)
                if (across_slot(a, ir_operand_used_value(in, o), &slot))
                    across_set(use, slot);
        }
    }
}

static void
across_edge_uses(across_t *a, const ir_block_t *from, const ir_block_t *to,
                 uint64_t *bits)
{
    for (uint32_t i = 0; i < to->n_instrs && to->instrs[i]->op == IR_OP_PHI;
         i++) {
        const ir_instr_t *phi = to->instrs[i];
        for (uint32_t o = 0; o < phi->n_operands; o++) {
            const ir_operand_t *operand = &phi->operands[o];
            size_t slot = 0;
            if (operand->kind == IR_OPD_PHI_ARM && operand->phi.block == from
                && across_slot(a, operand->phi.value, &slot))
                across_set(bits, slot);
        }
    }
}

static void
across_solve(across_t *a)
{
    ir_func_t *f = a->live->f;
    size_t words = a->words;
    uint64_t *next = core_alloc_array(uint64_t, words ? words : 1);
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = a->live->count; b-- > 0;) {
            ir_block_t *block = f->blocks[b];
            ir_instr_t *terminator = ir_block_terminator(block);
            uint64_t *out = a->out + (size_t)b * words;
            for (size_t w = 0; w < words; w++) next[w] = 0;
            for (uint32_t s = 0; s < ir_succ_count(terminator); s++) {
                ir_block_t *succ = ir_succ_at(terminator, s);
                uint64_t *in = a->in + (size_t)succ->index * words;
                for (size_t w = 0; w < words; w++) next[w] |= in[w];
                across_edge_uses(a, block, succ, next);
            }
            uint64_t *in = a->in + (size_t)b * words;
            uint64_t *use = a->use + (size_t)b * words;
            uint64_t *def = a->def + (size_t)b * words;
            for (size_t w = 0; w < words; w++) {
                uint64_t live_in = use[w] | (next[w] & ~def[w]);
                if (next[w] != out[w] || live_in != in[w]) changed = true;
                out[w] = next[w];
                in[w] = live_in;
            }
        }
    }
    core_free(next);
}

bool
ir_live_across_points(ir_live_t *live, ir_value_t *const *values,
                      size_t count, ir_live_point_fn point, void *context,
                      bool *across)
{
    if (!live || !live->cfg_valid || !point || (count && !across))
        return false;
    for (size_t v = 0; v < count; v++) across[v] = false;
    if (!count) return true;
    for (size_t m = 0; m < live->across.len; m++) {
        across_memo_t *memo = &live->across.data[m];
        if (memo->point == point && memo->context == context
            && memo->count == count
            && !memcmp(memo->values, values, count * sizeof(*values))) {
            memcpy(across, memo->across, count * sizeof(*across));
            return true;
        }
    }
    across_t a = {.live = live, .words = (count + 63) / 64};
    core_dict_init(&a.slots, core_hash_word, nullptr);
    for (size_t v = 0; v < count; v++)
        core_dict_put(&a.slots, values[v], (void *)(uintptr_t)(v + 1));
    size_t cells = (size_t)live->count * a.words;
    a.use = core_alloc_array(uint64_t, cells);
    a.def = core_alloc_array(uint64_t, cells);
    a.in = core_alloc_array(uint64_t, cells);
    a.out = core_alloc_array(uint64_t, cells);
    across_local_sets(&a);
    across_solve(&a);
    uint64_t *current = core_alloc_array(uint64_t, a.words);
    uint64_t *hit = core_alloc_array(uint64_t, a.words);
    for (uint32_t b = 0; b < live->count; b++) {
        ir_block_t *block = live->f->blocks[b];
        uint64_t *out = a.out + (size_t)b * a.words;
        for (size_t w = 0; w < a.words; w++) current[w] = out[w];
        for (uint32_t i = block->n_instrs; i-- > 0;) {
            const ir_instr_t *in = block->instrs[i];
            size_t slot = 0;
            bool defines = across_slot(&a, in->dest, &slot);
            if (defines) across_clear(current, slot);
            if (point(context, in))
                for (size_t w = 0; w < a.words; w++) hit[w] |= current[w];
            if (in->op == IR_OP_PHI) continue;
            for (uint32_t o = 0; o < in->n_operands; o++)
                if (across_slot(&a, ir_operand_used_value(in, o), &slot))
                    across_set(current, slot);
        }
    }
    for (size_t v = 0; v < count; v++)
        across[v] = (hit[v / 64] >> (v % 64)) & 1;
    across_memo_t memo = {
        .point = point, .context = context, .count = count,
        .values = core_alloc_array(ir_value_t *, count),
        .across = core_alloc_array(bool, count),
    };
    memcpy(memo.values, values, count * sizeof(*values));
    memcpy(memo.across, across, count * sizeof(*across));
    core_list_push(live->across, memo);
    core_free(hit);
    core_free(current);
    core_free(a.use);
    core_free(a.def);
    core_free(a.in);
    core_free(a.out);
    core_dict_free(&a.slots);
    return true;
}

static void
compute_cycles(ir_live_t *live)
{
    uint32_t count = live->count;
    live->cycle = core_alloc_array(int8_t, count);
    uint32_t *index = core_alloc_array(uint32_t, count);
    uint32_t *low = core_alloc_array(uint32_t, count);
    bool *on_stack = core_alloc_array(bool, count);
    uint32_t *stack = core_alloc_array(uint32_t, count);
    uint32_t *frames = core_alloc_array(uint32_t, count);
    uint32_t *next_succ = core_alloc_array(uint32_t, count);
    uint32_t counter = 1, top = 0, depth = 0;
    for (uint32_t root = 0; root < count; root++) {
        if (index[root]) continue;
        frames[depth++] = root;
        index[root] = low[root] = counter++;
        stack[top++] = root;
        on_stack[root] = true;
        while (depth) {
            uint32_t b = frames[depth - 1];
            ir_instr_t *terminator = ir_block_terminator(live->f->blocks[b]);
            uint32_t successors = ir_succ_count(terminator);
            if (next_succ[b] < successors) {
                uint32_t s = ir_succ_at(terminator, next_succ[b]++)->index;
                if (s == b) live->cycle[b] = 1;
                if (!index[s]) {
                    index[s] = low[s] = counter++;
                    stack[top++] = s;
                    on_stack[s] = true;
                    frames[depth++] = s;
                } else if (on_stack[s] && index[s] < low[b])
                    low[b] = index[s];
                continue;
            }
            depth--;
            if (depth && low[b] < low[frames[depth - 1]])
                low[frames[depth - 1]] = low[b];
            if (low[b] != index[b]) continue;
            uint32_t members = 0, member;
            uint32_t base = top;
            do {
                member = stack[--base];
                members++;
            } while (member != b);
            for (uint32_t m = base; m < top; m++) {
                on_stack[stack[m]] = false;
                if (members > 1) live->cycle[stack[m]] = 1;
                else if (!live->cycle[stack[m]]) live->cycle[stack[m]] = -1;
            }
            top = base;
        }
    }
    core_free(index);
    core_free(low);
    core_free(on_stack);
    core_free(stack);
    core_free(frames);
    core_free(next_succ);
}

bool
ir_live_block_on_cycle(ir_live_t *live, const ir_block_t *block)
{
    if (!live || !live->cfg_valid || !ir_block_belongs_to(live->f, block))
        return true;
    if (!live->cycle) compute_cycles(live);
    return live->cycle[block->index] > 0;
}

static void
build_uses(ir_live_t *live)
{
    ir_func_t *f = live->f;
    size_t total = 0;
    core_list_t(uint32_t) counts = core_list_new(uint32_t);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            if (in->op == IR_OP_COPY && in->dest_local) {
                ir_value_t *local = ir_local_value(f, in->dest_local);
                bool known = false;
                core_list_t(live_copy_t) *copies = local
                    ? core_dict_get(&live->copies, local, &known) : nullptr;
                if (local && !known) {
                    copies = core_alloc(core_list_t(live_copy_t));
                    *copies = core_list_new(live_copy_t);
                    core_dict_put(&live->copies, local, copies);
                }
                if (copies) core_list_push(*copies, in);
            }
            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_value_t *value = ir_operand_used_value(in, o);
                if (!value) continue;
                bool found = false;
                void *slot = core_dict_get(&live->use_index,
                                           (void *)use_key(value), &found);
                if (!found) {
                    slot = (void *)(uintptr_t)counts.len;
                    core_dict_put(&live->use_index, use_key(value),
                                  counts.len);
                    core_list_push(counts, 0);
                }
                counts.data[(uintptr_t)slot]++;
                total++;
            }
        }
    live->use_start = core_alloc_array(uint32_t, counts.len + 1);
    for (size_t v = 0; v < counts.len; v++)
        live->use_start[v + 1] = live->use_start[v] + counts.data[v];
    live->uses = core_alloc_array(live_use_t, total ? total : 1);
    for (size_t v = 0; v < counts.len; v++) counts.data[v] = 0;
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_value_t *value = ir_operand_used_value(in, o);
                if (!value) continue;
                bool found = false;
                uintptr_t slot = (uintptr_t)core_dict_get(&live->use_index,
                    (void *)use_key(value), &found);
                live->uses[live->use_start[slot] + counts.data[slot]++] =
                    (live_use_t){in, o};
            }
        }
    core_list_free(counts);
    live->uses_built = true;
}

ir_liveness_t
ir_live_carried_after(ir_live_t *live, const ir_value_t *root,
                      ir_live_flow_fn flow, void *context,
                      const ir_instr_t *point)
{
    if (!live || !live->cfg_valid || !root) return IR_LIVENESS_UNKNOWN;
    if (!live->uses_built) build_uses(live);
    core_dict_t seen;
    core_dict_init(&seen, core_hash_word, nullptr);
    size_t depth = 0, capacity = 16;
    ir_liveness_t state = IR_LIVENESS_DEAD;
    core_dict_put(&seen, root, 1);
    const ir_value_t **work = core_alloc_array(const ir_value_t *, capacity);
    work[depth++] = root;
    while (depth && state == IR_LIVENESS_DEAD) {
        const ir_value_t *value = work[--depth];
        if (value != root && reaches_after(live, value, point)) {
            state = IR_LIVENESS_LIVE;
            break;
        }
        bool found = false;
        uintptr_t slot = (uintptr_t)core_dict_get(&live->use_index,
                                                  (void *)use_key(value),
                                                  &found);
        for (uint32_t u = found ? live->use_start[slot] : 0;
             found && u < live->use_start[slot + 1]; u++) {
            live_use_t use = live->uses[u];
            ir_live_flow_t kind = flow(context, use.user, use.operand);
            if (kind == IR_LIVE_FLOW_HOLD) {
                state = IR_LIVENESS_UNKNOWN;
                break;
            }
            const ir_value_t *carried = use.user->dest;
            if (kind != IR_LIVE_FLOW_CARRY || !carried) continue;
            bool known = false;
            core_dict_get(&seen, carried, &known);
            if (known) continue;
            core_dict_put(&seen, carried, 1);
            if (depth == capacity) {
                const ir_value_t **grown = core_alloc_array(
                    const ir_value_t *, capacity * 2);
                for (size_t i = 0; i < depth; i++) grown[i] = work[i];
                core_free(work);
                work = grown;
                capacity *= 2;
            }
            work[depth++] = carried;
        }
    }
    core_free(work);
    core_dict_free(&seen);
    return state;
}
