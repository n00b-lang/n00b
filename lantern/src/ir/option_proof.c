#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/option_proof.h"
#include "ir/pdg.h"
#include "ir/walk.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#define MAX_PATH 16

typedef enum : uint8_t {
    ROOT_NONE,
    ROOT_VALUE,
    ROOT_LOCAL,
    ROOT_CELL,
    ROOT_REFERENCE,
} root_kind_t;

typedef struct {
    root_kind_t   kind;
    ir_value_t   *value;
    ir_local_t   *local;
    ir_instr_t   *read;
    core_string_t fields[MAX_PATH];
    uint32_t      depth;
} option_key_t;

typedef struct {
    ir_func_t  *f;
    ir_pdg_t   *pdg;
    ir_doms_t  *dom;
    uint32_t   *base;
    uint32_t    total;
    uint8_t    *forward;
    uint8_t    *backward;
} proof_t;

static option_key_t key_of(ir_value_t *v)
{
    option_key_t key = {0};
    core_string_t reversed[MAX_PATH];
    uint32_t depth = 0;

    while (v) {
        ir_instr_t *def = v->def;

        if (v->kind == IR_VAL_LOCAL) {
            key.kind  = ROOT_LOCAL;
            key.local = v->local;
            break;
        }
        if (!def) {
            key.kind  = ROOT_VALUE;
            key.value = v;
            break;
        }
        if (def->op == IR_OP_EXTRACT && def->n_operands == 2
            && def->operands[1].kind == IR_OPD_NAME && depth < MAX_PATH) {
            reversed[depth++] = def->operands[1].name;
            v = ir_operand_value(&def->operands[0]);
            continue;
        }
        if (def->op == IR_OP_OPTION_GET && def->n_operands == 1
            && depth < MAX_PATH) {
            reversed[depth++] = CORE_STRING_STATIC("!");
            v = ir_operand_value(&def->operands[0]);
            continue;
        }
        if (def->op == IR_OP_COPY && def->n_operands == 1) {
            ir_value_t *source = ir_operand_value(&def->operands[0]);

            if (source && source->kind == IR_VAL_LOCAL) {
                key.kind  = ROOT_LOCAL;
                key.local = source->local;
                key.read  = def;
                break;
            }
            v = source;
            continue;
        }
        if (def->op == IR_OP_LOAD && def->n_operands == 1) {
            ir_value_t *cell = ir_operand_value(&def->operands[0]);

            if (cell && cell->def && cell->def->op == IR_OP_ALLOC) {
                key.kind  = ROOT_CELL;
                key.value = cell;
                key.read  = def;
                break;
            }
            if (cell && cell->kind == IR_VAL_PARAM) {
                key.kind  = ROOT_REFERENCE;
                key.value = cell;
                key.read  = def;
                break;
            }
        }
        key.kind  = ROOT_VALUE;
        key.value = v;
        break;
    }
    for (uint32_t i = 0; i < depth; i++) {
        key.fields[i] = reversed[depth - 1 - i];
    }
    key.depth = depth;
    return key;
}

static bool same_key(const option_key_t *a, const option_key_t *b)
{
    if (a->kind != b->kind || a->depth != b->depth || a->kind == ROOT_NONE) {
        return false;
    }
    if (a->kind == ROOT_LOCAL ? a->local != b->local : a->value != b->value) {
        return false;
    }
    for (uint32_t i = 0; i < a->depth; i++) {
        if (!core_string_eq(a->fields[i], b->fields[i])) {
            return false;
        }
    }
    return true;
}

static bool constant_bool(ir_value_t *v, bool *out)
{
    ir_instr_t *def = v ? v->def : nullptr;

    if (!def || def->op != IR_OP_CONST || def->n_operands != 1
        || def->operands[0].kind != IR_OPD_CONST
        || def->operands[0].lit.is_float) {
        return false;
    }
    *out = def->operands[0].lit.i.mag != 0;
    return true;
}

static bool proven_value(ir_value_t *condition, bool truth, option_key_t *out)
{
    for (uint32_t guard = 0; condition && guard < 64; guard++) {
        ir_instr_t *def = condition->def;

        if (!def) {
            return false;
        }
        if (def->op == IR_OP_OPTION_IS_NIL && def->n_operands == 1) {
            if (truth) {
                return false;
            }
            *out = key_of(ir_operand_value(&def->operands[0]));
            return out->kind != ROOT_NONE;
        }
        if (def->op == IR_OP_NOT && def->n_operands == 1) {
            truth     = !truth;
            condition = ir_operand_value(&def->operands[0]);
            continue;
        }
        if (def->op == IR_OP_COPY && def->n_operands == 1) {
            condition = ir_operand_value(&def->operands[0]);
            if (condition && condition->kind == IR_VAL_LOCAL) {
                return false;
            }
            continue;
        }
        if ((def->op == IR_OP_EQ || def->op == IR_OP_NE)
            && def->n_operands == 2) {
            ir_value_t *a = ir_operand_value(&def->operands[0]);
            ir_value_t *b = ir_operand_value(&def->operands[1]);
            bool k = false;

            if (!constant_bool(b, &k)) {
                if (!constant_bool(a, &k)) {
                    return false;
                }
                a = b;
            }
            bool equal = def->op == IR_OP_EQ ? truth : !truth;
            truth      = equal ? k : !k;
            condition  = a;
            continue;
        }
        return false;
    }
    return false;
}

static uint32_t instr_id(const proof_t *p, const ir_instr_t *in)
{
    return p->base[in->block->index] + in->index;
}

static void visit(proof_t *p, uint8_t *set, ir_instr_t *in,
                  ir_instr_t *barrier, ir_instr_t **stack, uint32_t *top)
{
    if (!in || in == barrier) {
        return;
    }
    uint32_t id = instr_id(p, in);

    if (set[id]) {
        return;
    }
    set[id] = 1;
    stack[(*top)++] = in;
}

static void reach(proof_t *p, uint8_t *set, ir_instr_t *from,
                  ir_instr_t *barrier, bool forward)
{
    ir_instr_t **stack = core_alloc_size(sizeof(ir_instr_t *),
                                         p->total + 1);
    uint32_t top = 0;

    memset(set, 0, p->total ? p->total : 1);
    if (!stack) {
        return;
    }
    stack[top++] = from;
    while (top) {
        ir_instr_t *in = stack[--top];
        ir_block_t *b  = in->block;

        if (forward && in->index + 1 < b->n_instrs) {
            visit(p, set, b->instrs[in->index + 1], barrier, stack, &top);
        } else if (forward) {
            for (uint32_t s = 0; s < ir_succ_count(in); s++) {
                ir_block_t *next = ir_succ_at(in, s);

                if (next && next->n_instrs) {
                    visit(p, set, next->instrs[0], barrier, stack, &top);
                }
            }
        } else if (in->index) {
            visit(p, set, b->instrs[in->index - 1], barrier, stack, &top);
        } else {
            for (uint32_t k = 0; k < ir_pred_count(p->dom, b); k++) {
                ir_block_t *prev = ir_pred_at(p->dom, b, k);

                if (prev && prev->n_instrs) {
                    visit(p, set, prev->instrs[prev->n_instrs - 1], barrier,
                          stack, &top);
                }
            }
        }
    }
    core_free(stack);
}

static bool writes_root(const option_key_t *key, ir_instr_t *in)
{
    if (key->kind == ROOT_LOCAL) {
        if (in->dest_kind == IR_DEST_LOCAL && in->dest_local == key->local) {
            return true;
        }
        if (key->local->mutability != IR_MUT_MUTABLE) {
            return false;
        }
        for (uint32_t i = 0; i < in->n_operands; i++) {
            if (ir_operand_local(&in->operands[i]) == key->local
                && (in->operands[i].kind == IR_OPD_OUT
                    || in->op == IR_OP_ADDRESS_OF)) {
                return true;
            }
        }
        return key->local->address_taken
            && (ir_op_info(in->op)->flags & IR_OPF_SIDEEFFECT);
    }
    return in->op == IR_OP_STORE && in->n_operands == 2
        && ir_operand_value(&in->operands[0]) == key->value;
}

static bool cell_escapes(ir_func_t *f, ir_value_t *cell)
{
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];

        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];

            for (uint32_t o = 0; o < in->n_operands; o++) {
                if (ir_operand_value(&in->operands[o]) != cell) {
                    continue;
                }
                bool load  = in->op == IR_OP_LOAD && o == 0;
                bool store = in->op == IR_OP_STORE && o == 0;

                if (!load && !store) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool stable(proof_t *p, const option_key_t *key, ir_instr_t *test,
                   ir_instr_t *use)
{
    if (key->kind == ROOT_VALUE) {
        return true;
    }
    if (key->kind == ROOT_REFERENCE || !test || !use) {
        return false;
    }
    if (key->kind == ROOT_CELL && cell_escapes(p->f, key->value)) {
        return false;
    }
    reach(p, p->forward, test, test, true);
    reach(p, p->backward, use, test, false);
    for (uint32_t b = 0; b < p->f->n_blocks; b++) {
        ir_block_t *block = p->f->blocks[b];

        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            uint32_t id = instr_id(p, in);

            if (p->forward[id] && p->backward[id] && writes_root(key, in)) {
                return false;
            }
        }
    }
    return true;
}

static bool controlled_proof(proof_t *p, ir_instr_t *get,
                             const option_key_t *key)
{
    ir_block_t **work = core_alloc_size(sizeof(ir_block_t *),
                                        p->f->n_blocks + 1);
    uint8_t *seen = core_alloc_size(1, p->f->n_blocks + 1);
    uint32_t top = 0;
    bool proven = false;

    if (!work || !seen) {
        core_free(work);
        core_free(seen);
        return false;
    }
    memset(seen, 0, p->f->n_blocks + 1);
    work[top++] = get->block;
    seen[get->block->index] = 1;
    while (top && !proven) {
        ir_block_t *b = work[--top];

        for (uint32_t c = 0; c < ir_controllers_of(p->pdg, b) && !proven;
             c++) {
            ir_block_t *on   = ir_controller_at(p->pdg, b, c);
            uint32_t    slot = ir_controller_slot_at(p->pdg, b, c);
            ir_instr_t *t    = on ? ir_block_terminator(on) : nullptr;

            if (!on) {
                continue;
            }
            if (!seen[on->index]) {
                seen[on->index] = 1;
                work[top++] = on;
            }
            if (!t || t->op != IR_OP_BRANCH || slot >= t->n_operands) {
                continue;
            }
            if (slot != 1 && slot != 2) {
                continue;
            }
            ir_block_t *target = ir_succ_of(&t->operands[slot]);
            option_key_t tested = {0};

            proven = target && ir_pred_count(p->dom, target) == 1
                && ir_dominates(p->dom, target, get->block)
                && proven_value(ir_operand_value(&t->operands[0]),
                                slot == 1, &tested)
                && same_key(&tested, key)
                && stable(p, key, tested.read, key->read);
        }
    }
    core_free(work);
    core_free(seen);
    return proven;
}

static ir_value_t *cell_source(ir_func_t *f, ir_value_t *cell)
{
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];

        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];

            if (in->op == IR_OP_STORE && in->n_operands == 2
                && ir_operand_value(&in->operands[0]) == cell) {
                ir_value_t *stored = ir_operand_value(&in->operands[1]);

                return stored && stored->kind == IR_VAL_PARAM ? stored
                                                              : nullptr;
            }
        }
    }
    return nullptr;
}

static core_string_t key_name(ir_func_t *f, const option_key_t *key)
{
    core_buffer_t name = {0};
    ir_value_t *param = (key->kind == ROOT_VALUE
                         || key->kind == ROOT_REFERENCE)
            && key->value->kind == IR_VAL_PARAM ? key->value
        : key->kind == ROOT_CELL ? cell_source(f, key->value) : nullptr;

    if (key->kind == ROOT_LOCAL && key->local->source_name.data) {
        core_buffer_append_string(&name, key->local->source_name);
    } else if (param && param->name.data) {
        core_buffer_append_string(&name, param->name);
    } else {
        core_buffer_release(&name);
        return (core_string_t){0};
    }
    for (uint32_t i = 0; i < key->depth; i++) {
        if (core_string_eq(key->fields[i], CORE_STRING_STATIC("!"))) {
            continue;
        }
        core_buffer_append_string(&name, CORE_STRING_STATIC("."));
        core_buffer_append_string(&name, key->fields[i]);
    }
    core_string_t result = core_string_dup(core_buffer_string(&name));
    core_buffer_release(&name);
    return result;
}

static void report(ir_diags_t *d, proof_t *p, ir_instr_t *get,
                   const option_key_t *key)
{
    ir_diag_t *dg = ir_diag_begin_at(d, IR_DIAG_UNPROVEN_OPTION, get,
                                     get->block, p->f,
                                     CORE_STRING_STATIC("this use"));
    if (!dg) {
        return;
    }
    core_string_t name = key_name(p->f, key);
    core_buffer_t message = {0};

    if (name.data && key->kind == ROOT_REFERENCE) {
        core_buffer_append_string(&message, CORE_STRING_STATIC("'"));
        core_buffer_append_string(&message, name);
        core_buffer_append_string(&message, CORE_STRING_STATIC(
            "' is read through a reference, so a test against nil does not "
            "keep it non-nil; copy it into a local with `let` and test the "
            "local"));
    } else if (name.data) {
        core_buffer_append_string(&message, CORE_STRING_STATIC("'"));
        core_buffer_append_string(&message, name);
        core_buffer_append_string(&message, CORE_STRING_STATIC(
            "' may be nil here; use its value only where a test such as `"));
        core_buffer_append_string(&message, name);
        core_buffer_append_string(&message, CORE_STRING_STATIC(
            " != nil` proves it, with no assignment to it in between"));
    } else {
        core_buffer_append_string(&message, CORE_STRING_STATIC(
            "this option may be nil here; store it in a variable and test "
            "the variable against nil before using its value"));
    }
    ir_diag_message(dg, d, core_buffer_string(&message));
    core_buffer_release(&message);
    core_free(name.data);
}

static void check_func(ir_func_t *f, ir_diags_t *d)
{
    if (!f->n_blocks) {
        return;
    }
    size_t  scope = ir_analysis_scope_begin();
    proof_t p = {.f = f, .pdg = ir_func_pdg(f), .dom = ir_func_doms(f)};

    p.base = core_alloc_size(sizeof(uint32_t), f->n_blocks + 1);
    if (!p.pdg || !p.dom || !p.base) {
        core_free(p.base);
        ir_analysis_scope_end(scope);
        return;
    }
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        p.base[b] = p.total;
        p.total += f->blocks[b]->n_instrs;
    }
    p.forward  = core_alloc_size(1, p.total ? p.total : 1);
    p.backward = core_alloc_size(1, p.total ? p.total : 1);
    for (uint32_t b = 0; p.forward && p.backward && b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];

        if (!ir_block_reachable(p.dom, block)) {
            continue;
        }
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];

            if (in->op != IR_OP_OPTION_GET || in->n_operands != 1) {
                continue;
            }
            option_key_t key = key_of(ir_operand_value(&in->operands[0]));

            if (!controlled_proof(&p, in, &key)) {
                report(d, &p, in, &key);
            }
        }
    }
    core_free(p.forward);
    core_free(p.backward);
    core_free(p.base);
    ir_analysis_scope_end(scope);
}

static bool check_visit(ir_visit_t *vi)
{
    check_func(vi->func, vi->user);
    return false;
}

bool ir_check_option_proofs(ir_module_t *m, ir_diags_t *d)
{
    if (!m || !d) {
        return false;
    }
    ir_walk_t *w = ir_walk_new(d);

    ir_walk_on_func(w, check_visit, nullptr, nullptr);
    ir_walk_run(w, m);
    ir_walk_free(w);
    return !ir_diags_failed(d);
}
