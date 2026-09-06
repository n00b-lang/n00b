#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/rule.h"
#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/text.h"
#include "ir/verify.h"

typedef struct {
    const void **at;
    size_t       cap;
} pset;

static size_t pset_slot(const pset *s, const void *p)
{
    size_t h = (size_t)((uintptr_t)p >> 4) * 11400714819323198485u;
    size_t i = h & (s->cap - 1);

    while (s->at[i] && s->at[i] != p) {
        i = (i + 1) & (s->cap - 1);
    }

    return i;
}

static void pset_init(ir_ctx_t *ctx, pset *s, size_t want)
{
    s->cap = 16;

    while (s->cap < want * 2) {
        s->cap *= 2;
    }
    s->at = ir_alloc(ctx, s->cap * sizeof(void *));
}

static void pset_add(pset *s, const void *p)
{
    if (p) {
        s->at[pset_slot(s, p)] = p;
    }
}

static bool pset_has(const pset *s, const void *p)
{
    return p && s->at[pset_slot(s, p)] == p;
}

typedef struct {
    ir_module_t *m;
    ir_diags_t  *d;
    bool         bad;
    ir_func_t   *dom_for;
    ir_doms_t   *dom;
    pset         checked;
} V;

static ir_doms_t *doms(V *v, ir_func_t *f)
{
    if (v->dom_for != f) {
        v->dom     = ir_doms_of(f);
        v->dom_for = f;
    }

    return v->dom;
}

__attribute__((format(printf, 6, 7))) static void
broke(V *v, ir_diagcode_t code, ir_instr_t *in, ir_block_t *b,
      const char *rule, const char *fmt, ...)
{
    v->bad = true;

    if (!v->d) {
        return;
    }

    ir_func_t *f  = in && in->block ? in->block->owner
                  : b               ? b->owner
                                    : nullptr;
    ir_diag_t *dg = ir_diag_begin_at(v->d, code, in, b, f, rule);

    if (!dg) {
        return;
    }

    ir_sb   sb = {0};
    va_list ap;

    va_start(ap, fmt);
    ir_sb_vfmt(&sb, fmt, ap);
    va_end(ap);

    char *msg = ir_sb_take(&sb);

    ir_diag_message(dg, v->d, msg ? msg : rule);
    free(msg);
}

static bool names_block(ir_func_t *f, ir_block_t *b)
{
    return b && b->index < f->n_blocks && f->blocks[b->index] == b;
}

static bool is_phi(ir_instr_t *in)
{
    return in && in->op == IR_OP_PHI;
}

static void check_origins(V *v, ir_origin_t *o, ir_instr_t *in, ir_block_t *b)
{
    ir_origin_t *slow = o;

    for (uint32_t n = 0; o; n++) {
        if (o->kind != IR_ORIGIN_SOURCE && !o->agent) {
            broke(v, IR_DIAG_VERIFY_ORIGIN, in, b, "origin.agent",
                  "an origin of kind %s names no agent",
                  ir_origin_kind_name(o->kind));
        }

        o = o->parent;

        if (n & 1) {
            slow = slow->parent;
        }

        if (o && o == slow) {
            broke(v, IR_DIAG_VERIFY_ORIGIN, in, b, "origin.acyclic",
                  "the origin chain loops back on itself");

            return;
        }
    }
}

static void check_annots(V *v, ir_annots_t *a, ir_instr_t *in, ir_block_t *b)
{
    for (uint32_t i = 0; i < ir_annot_count(a); i++) {
        const char *key = ir_annot_at(a, i)->key;

        if (!ir_annot_key_ok(key, v->m->profile ? v->m->profile->name : "")) {
            broke(v, IR_DIAG_BAD_ANNOT_KEY, in, b, "annot.namespace",
                  "the annotation key %s is not namespaced by ir or by the "
                  "profile", key);
        }
    }
}

static void check_parsed(V *v, ir_func_t *f)
{
    if (f->n_blocks && ir_pred_count(doms(v, f), ir_block_entry(f))) {
        broke(v, IR_DIAG_VERIFY_PARSED, nullptr, ir_block_entry(f),
              "parsed.entry", "the entry block of %s has a predecessor",
              f->name);
    }

    uint32_t *named = ir_alloc(v->m->ctx,
                               (f->n_blocks + 1) * sizeof(uint32_t));

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *b     = f->blocks[i];
        uint32_t    terms = 0;
        bool        run   = true;

        check_annots(v, &b->annots, nullptr, b);
        check_origins(v, b->origin, nullptr, b);

        for (uint32_t k = 0; k < b->n_instrs; k++) {
            ir_instr_t *in = b->instrs[k];

            check_annots(v, &in->annots, in, b);
            check_origins(v, in->origin, in, b);

            if (ir_op_is_terminator(in->op)) {
                terms++;

                if (k + 1 != b->n_instrs) {
                    broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.terminator",
                          "%s terminates block %s but is not its last "
                          "instruction",
                          ir_module_op_spelling(v->m, in->op), b->name);
                }
            }

            if (is_phi(in) && !run) {
                broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.phi.leading",
                      "a phi follows a non-phi instruction in block %s",
                      b->name);
            }
            run = run && is_phi(in);

            const ir_opinfo_t *info = ir_module_op_info(v->m, in->op);

            if (!info) {
                continue;
            }

            if (!ir_opinfo_accepts_operands(info, in->n_operands)) {
                broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.arity",
                      "%s takes %u to %u operands and has %u", info->spelling,
                      info->min_operands,
                      info->max_operands == IR_VAR_ARITY ? info->min_operands
                                                         : info->max_operands,
                      in->n_operands);
            }

            for (uint32_t o = 0; o < in->n_operands; o++) {
                ir_opdkind_t want = ir_opinfo_operand_kind(info, o);
                ir_opdkind_t got  = in->operands[o].kind;
                bool         fits = got == want
                                 || (want == IR_OPD_ARG
                                     && (got == IR_OPD_VALUE
                                         || got == IR_OPD_NAMED_ARG));

                if (!fits) {
                    broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.operand",
                          "operand %u of %s is a %s where the table says %s",
                          o, info->spelling, ir_opdkind_name(got),
                          ir_opdkind_name(want));
                }
            }

            for (uint32_t o = 0; o < in->n_mods; o++) {
                if (!ir_opinfo_accepts_mod(info, in->mods[o])) {
                    broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.modifier",
                          "%s does not take the modifier %s", info->spelling,
                          in->mods[o]);
                }
            }

            for (uint32_t o = 0; o < in->n_operands; o++) {
                ir_operand_t *od = &in->operands[o];
                ir_block_t   *to = od->kind == IR_OPD_BLOCK         ? od->block
                                 : od->kind == IR_OPD_DEFAULT_BLOCK ? od->block
                                 : od->kind == IR_OPD_SWITCH_ARM ? od->arm.block
                                 : od->kind == IR_OPD_PHI_ARM    ? od->phi.block
                                                                 : nullptr;

                if (to && !names_block(f, to)) {
                    broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.block-ref",
                          "block %s is not a block of %s", to->name, f->name);
                }
            }

            if (is_phi(in)) {
                ir_doms_t *dm = doms(v, f);

                for (uint32_t a = 0; a < in->n_operands; a++) {
                    if (in->operands[a].kind == IR_OPD_PHI_ARM
                        && in->operands[a].phi.block) {
                        named[in->operands[a].phi.block->index]++;
                    }
                }

                for (uint32_t p = 0; p < ir_pred_count(dm, b); p++) {
                    ir_block_t *pred = ir_pred_at(dm, b, p);

                    if (named[pred->index] != 1) {
                        broke(v, IR_DIAG_VERIFY_PARSED, in, b,
                              "parsed.phi.arms",
                              "this phi names block %s %u times and %s is a "
                              "predecessor once", pred->name,
                              named[pred->index], pred->name);
                    }
                    named[pred->index] = 0;
                }

                for (uint32_t a = 0; a < in->n_operands; a++) {
                    ir_block_t *arm = in->operands[a].kind == IR_OPD_PHI_ARM
                                        ? in->operands[a].phi.block
                                        : nullptr;

                    if (!arm || !named[arm->index]) {
                        continue;
                    }
                    named[arm->index] = 0;
                    broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.phi.arms",
                          "this phi names block %s, which is not a "
                          "predecessor", arm->name);
                }
            }
        }

        if (terms != 1) {
            broke(v, IR_DIAG_VERIFY_PARSED, nullptr, b, "parsed.terminator",
                  "block %s has %u terminators and needs exactly one", b->name,
                  terms);
        }
    }
}

static void check_typed(V *v, ir_func_t *f)
{
    ir_rules_t *rs = ir_module_rules(v->m);

    for (uint32_t i = 0; i < f->n_params; i++) {
        if (!f->params[i]->type) {
            broke(v, IR_DIAG_VERIFY_TYPED, nullptr, ir_block_entry(f),
                  "typed.value", "parameter %s of %s has no type",
                  f->params[i]->name, f->name);
        }
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *b = f->blocks[i];

        for (uint32_t k = 0; k < b->n_instrs; k++) {
            ir_instr_t *in = b->instrs[k];

            if (!ir_module_op_info(v->m, in->op)) {
                broke(v, IR_DIAG_MISSING_PROFILE, in, b, "typed.extension",
                      "this extension opcode is not one profile %s supplies",
                      v->m->profile ? v->m->profile->name : "(none)");
                continue;
            }

            for (uint32_t o = 0; o < in->n_operands; o++) {
                ir_value_t *val = in->operands[o].kind == IR_OPD_VALUE
                                    ? in->operands[o].value
                                    : nullptr;

                if (val && !val->type) {
                    broke(v, IR_DIAG_VERIFY_TYPED, in, b, "typed.value",
                          "operand %u has no type", o);
                }
            }

            ir_verdict_t verdict = ir_rule_apply(v->m, rs, in, v->d).verdict;

            if (verdict == IR_RULE_DECLINE) {
                broke(v, IR_DIAG_VERIFY_TYPED, in, b, "typed.rule",
                      "no typing rule resolves for %s with these operand "
                      "types", ir_module_op_spelling(v->m, in->op));
            }
            else if (verdict != IR_RULE_ACCEPT) {
                v->bad = true;
            }
        }
    }
}

static void check_ssa(V *v, ir_func_t *f)
{
    if (f->n_locals) {
        broke(v, IR_DIAG_VERIFY_SSA, nullptr, ir_block_entry(f), "ssa.locals",
              "%s is in SSA form and still declares %u locals", f->name,
              f->n_locals);
    }

    ir_doms_t *dom = doms(v, f);

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *b = f->blocks[i];

        for (uint32_t k = 0; k < b->n_instrs; k++) {
            ir_instr_t *in = b->instrs[k];

            if (in->dest_kind == IR_DEST_LOCAL) {
                broke(v, IR_DIAG_VERIFY_SSA, in, b, "ssa.locals",
                      "this instruction writes local $%s in SSA form",
                      in->dest_local ? in->dest_local->name : "?");
            }

            for (uint32_t o = 0; o < in->n_operands; o++) {
                ir_operand_t *od  = &in->operands[o];
                ir_value_t   *val = od->kind == IR_OPD_VALUE     ? od->value
                                  : od->kind == IR_OPD_NAMED_ARG ? od->named.value
                                  : od->kind == IR_OPD_PHI_ARM   ? od->phi.value
                                                                 : nullptr;

                if (!val || val->kind != IR_VAL_INSTR || !val->def
                    || !val->def->block) {
                    continue;
                }

                ir_block_t *from = od->kind == IR_OPD_PHI_ARM ? od->phi.block
                                                              : b;

                if (!ir_dominates(dom, val->def->block, from)) {
                    broke(v, IR_DIAG_VERIFY_SSA, in, b, "ssa.dominance",
                          "%%%s is used here but its definition in block %s "
                          "does not dominate %s", val->name,
                          val->def->block->name, from->name);
                    continue;
                }

                if (val->def->block == from && od->kind != IR_OPD_PHI_ARM
                    && val->def->index >= in->index) {
                    broke(v, IR_DIAG_VERIFY_SSA, in, b, "ssa.dominance",
                          "%%%s is used before it is defined in block %s",
                          val->name, b->name);
                }
            }
        }
    }
}

typedef struct {
    const char **at;
    size_t       cap;
} nset;

static size_t nset_slot(const nset *s, const char *k)
{
    size_t h = 1469598103934665603u;

    for (const unsigned char *p = (const unsigned char *)k; *p; p++) {
        h = (h ^ *p) * 1099511628211u;
    }

    size_t i = h & (s->cap - 1);

    while (s->at[i] && strcmp(s->at[i], k)) {
        i = (i + 1) & (s->cap - 1);
    }

    return i;
}

static void nset_init(ir_ctx_t *ctx, nset *s, size_t want)
{
    s->cap = 16;

    while (s->cap < want * 2) {
        s->cap *= 2;
    }
    s->at = ir_alloc(ctx, s->cap * sizeof(char *));
}

static bool nset_add(nset *s, const char *k)
{
    size_t i = nset_slot(s, k);

    if (s->at[i]) {
        return false;
    }
    s->at[i] = k;

    return true;
}

static void check_uses_resolve(V *v, ir_func_t *f)
{
    ir_ctx_t *ctx  = v->m->ctx;
    size_t    want = f->n_params + 1;
    pset      defined;
    pset      locals;

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        want += f->blocks[i]->n_instrs;
    }
    pset_init(ctx, &defined, want);
    pset_init(ctx, &locals, f->n_locals + 1);

    for (uint32_t i = 0; i < f->n_params; i++) {
        pset_add(&defined, f->params[i]);
    }

    for (uint32_t i = 0; i < f->n_locals; i++) {
        pset_add(&locals, f->locals[i]);
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        for (uint32_t k = 0; k < f->blocks[i]->n_instrs; k++) {
            pset_add(&defined, f->blocks[i]->instrs[k]);
        }
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *b = f->blocks[i];

        for (uint32_t k = 0; k < b->n_instrs; k++) {
            ir_instr_t *in = b->instrs[k];

            for (uint32_t o = 0; o < in->n_operands; o++) {
                ir_operand_t *od  = &in->operands[o];
                ir_value_t   *val = od->kind == IR_OPD_VALUE     ? od->value
                                  : od->kind == IR_OPD_NAMED_ARG ? od->named.value
                                  : od->kind == IR_OPD_PHI_ARM   ? od->phi.value
                                                                 : nullptr;
                bool          ok  = true;

                if (!val) {
                    continue;
                }

                switch (val->kind) {
                case IR_VAL_GLOBAL:
                case IR_VAL_FUNC:
                    break;
                case IR_VAL_LOCAL:
                    ok = pset_has(&locals, val->local);
                    break;
                case IR_VAL_PARAM:
                    ok = pset_has(&defined, val);
                    break;
                default:
                    ok = pset_has(&defined, val->def);
                    break;
                }

                if (!ok) {
                    broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.value-ref",
                          "%%%s is not a parameter, local or instruction of %s",
                          val->name ? val->name : "?", f->name);
                }
            }
        }
    }
}

static void check_defined_once(V *v, ir_func_t *f)
{
    ir_ctx_t *ctx  = v->m->ctx;
    size_t    want = f->n_params + 1;
    nset      seen;

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        want += f->blocks[i]->n_instrs;
    }
    nset_init(ctx, &seen, want);

    for (uint32_t i = 0; i < f->n_params; i++) {
        nset_add(&seen, f->params[i]->name);
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *b = f->blocks[i];

        for (uint32_t k = 0; k < b->n_instrs; k++) {
            ir_instr_t *in = b->instrs[k];

            if (in->dest_kind != IR_DEST_VALUE || !in->dest) {
                continue;
            }

            if (!nset_add(&seen, in->dest->name)) {
                broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.one-def",
                      "%%%s is defined more than once in %s, or shadows a "
                      "parameter", in->dest->name, f->name);
            }
        }
    }
}

static bool settled_for(V *v, th_type *t, uint64_t *bytes)
{
    ir_descriptor_t *d = ir_layout_of(v->m, t, nullptr);

    if (!d || !ir_layout_is_settled(&d->size)) {
        return false;
    }
    *bytes = d->size.value;

    return true;
}

typedef struct {
    uint64_t    at;
    uint64_t    end;
    const char *name;
} mspan;

static int by_offset(const void *a, const void *b)
{
    const mspan *x = a;
    const mspan *y = b;

    return x->at < y->at ? -1 : x->at > y->at ? 1 : 0;
}

static void check_laid_out_type(V *v, th_type *t, ir_instr_t *in,
                                ir_block_t *b)
{
    if (!t) {
        return;
    }

    if (!v->checked.cap) {
        pset_init(v->m->ctx, &v->checked, ir_type_count(v->m) + 1);
    }

    if (pset_has(&v->checked, t)) {
        return;
    }
    pset_add(&v->checked, t);

    ir_descriptor_t *d = ir_layout_of(v->m, t, nullptr);

    if (!d) {
        broke(v, IR_DIAG_VERIFY_LAID_OUT, in, b, "laid-out.descriptor",
              "%s has no descriptor for target %s", ir_type_text(v->m, t),
              v->m->target ? v->m->target : "(none)");

        return;
    }

    if (!ir_layout_is_settled(&d->size) || !ir_layout_is_settled(&d->align)) {
        broke(v, IR_DIAG_VERIFY_LAID_OUT, in, b, "laid-out.descriptor",
              "the %s of %s is still %s for target %s",
              ir_layout_is_settled(&d->size) ? "alignment" : "size",
              ir_type_text(v->m, t),
              ir_layout_state_name(ir_layout_is_settled(&d->size)
                                       ? d->align.state
                                       : d->size.state),
              v->m->target ? v->m->target : "(none)");
    }

    if (d->variant.niche || d->n_members < 2) {
        return;
    }

    mspan   *s = ir_alloc(v->m->ctx, d->n_members * sizeof(mspan));
    uint32_t n = 0;

    for (uint32_t i = 0; i < d->n_members; i++) {
        const ir_member_layout_t *a = &d->members[i];

        if (!ir_layout_is_settled(&a->bit_offset)) {
            continue;
        }
        s[n++] = (mspan){.at   = a->bit_offset.value,
                         .end  = a->bit_offset.value + a->bit_width.value,
                         .name = a->name};
    }

    qsort(s, n, sizeof(mspan), by_offset);

    for (uint32_t i = 0; i + 1 < n; i++) {
        if (s[i].end > s[i + 1].at) {
            broke(v, IR_DIAG_VERIFY_LAID_OUT, in, b, "laid-out.overlap",
                  "members %s and %s of %s share bits, and no niche "
                  "encoding says they may", s[i].name, s[i + 1].name,
                  ir_type_text(v->m, t));
        }
    }
}

static void check_laid_out(V *v, ir_func_t *f)
{
    for (uint32_t i = 0; i < f->n_params; i++) {
        check_laid_out_type(v, f->params[i]->type, nullptr,
                            ir_block_entry(f));
    }

    for (uint32_t i = 0; i < f->n_locals; i++) {
        check_laid_out_type(v, f->locals[i]->type, nullptr, ir_block_entry(f));
    }

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *b = f->blocks[i];

        for (uint32_t k = 0; k < b->n_instrs; k++) {
            ir_instr_t *in = b->instrs[k];

            if (in->dest_kind == IR_DEST_VALUE && in->dest) {
                check_laid_out_type(v, in->dest->type, in, b);
            }

            for (uint32_t o = 0; o < in->n_operands; o++) {
                if (in->operands[o].kind == IR_OPD_TYPE) {
                    check_laid_out_type(v, in->operands[o].type, in, b);
                }

                if (in->operands[o].kind == IR_OPD_VALUE
                    && in->operands[o].value) {
                    check_laid_out_type(v, in->operands[o].value->type, in, b);
                }
            }

            if (in->op != IR_OP_BITCAST || !in->n_operands
                || in->operands[0].kind != IR_OPD_VALUE
                || !in->operands[0].value || !in->dest) {
                continue;
            }

            uint64_t from = 0;
            uint64_t to   = 0;

            if (settled_for(v, in->operands[0].value->type, &from)
                && settled_for(v, in->dest->type, &to) && from != to) {
                broke(v, IR_DIAG_VERIFY_LAID_OUT, in, b, "laid-out.bitcast",
                      "bitcast turns %llu bytes of %s into %llu bytes of %s",
                      (unsigned long long)from,
                      ir_type_text(v->m, in->operands[0].value->type),
                      (unsigned long long)to,
                      ir_type_text(v->m, in->dest->type));
            }
        }
    }
}

static void check_no_critical_edges(V *v, ir_func_t *f)
{
    ir_doms_t *dom = doms(v, f);

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *b = f->blocks[i];
        ir_instr_t *t = ir_block_terminator(b);

        if (ir_succ_count(t) < 2) {
            continue;
        }

        for (uint32_t s = 0; s < t->n_operands; s++) {
            ir_block_t *to = ir_succ_of(&t->operands[s]);

            if (to && ir_pred_count(dom, to) > 1) {
                broke(v, IR_DIAG_VERIFY_EDGE, t, b, "no-critical-edges.edge",
                      "the edge from %s to %s is critical: %s has several "
                      "successors and %s several predecessors", b->name,
                      to->name, b->name, to->name);
            }
        }
    }
}

static void tally(const ir_region_t *r, uint32_t *seen, uint32_t n)
{
    if (!r) {
        return;
    }

    if (r->kind == IR_REGION_BLOCK && r->head && r->head->index < n) {
        seen[r->head->index]++;
    }

    for (uint32_t i = 0; i < r->n_kids; i++) {
        tally(r->kids[i], seen, n);
    }
}

static void check_structured(V *v, ir_func_t *f)
{
    ir_region_t *root = ir_region_root(f);

    if (!root) {
        broke(v, IR_DIAG_VERIFY_STRUCTURE, nullptr, ir_block_entry(f),
              "structured.overlay", "%s has no region overlay", f->name);

        return;
    }

    uint32_t *seen = ir_alloc(v->m->ctx, f->n_blocks * sizeof(uint32_t));

    tally(root, seen, f->n_blocks);

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        if (seen[f->blocks[i]->index] != 1) {
            broke(v, IR_DIAG_VERIFY_STRUCTURE, nullptr, f->blocks[i],
                  "structured.overlay",
                  "the overlay covers block %s %u times, and must cover it "
                  "exactly once", f->blocks[i]->name,
                  seen[f->blocks[i]->index]);
        }
    }
}

static bool claims(ir_module_t *m, ir_props_t p)
{
    return (ir_module_props(m) & p) != 0;
}

static void audit_spans(V *v)
{
    if (!v->d) {
        return;
    }

    uint32_t n = ir_diags_count(v->d);

    for (uint32_t i = 0; i < n; i++) {
        const ir_diag_t *dg = ir_diags_at(v->d, i);

        if (ir_span_known(dg->primary.span) || dg->severity == IR_SEV_NOTE
            || dg->severity == IR_SEV_HELP) {
            continue;
        }

        ir_diag_t *bug = ir_diag_begin(v->d, IR_DIAG_NO_PRIMARY_SPAN,
                                       ir_module_span(v->m),
                                       "reported without a location");

        if (bug) {
            ir_diag_message(bug, v->d,
                            "a diagnostic was reported with no primary span, "
                            "which §11.3 makes a bug in whatever reported it");
        }

        return;
    }
}

ir_props_t ir_verify(ir_module_t *m, ir_diags_t *out)
{
    if (!m) {
        return 0;
    }

    V          v    = {.m = m, .d = out};
    ir_props_t held = 0;

    check_annots(&v, ir_module_annots(m), nullptr, nullptr);

    for (size_t i = 0; i < ir_func_count(m); i++) {
        ir_func_t *f = ir_func_at(m, i);

        check_origins(&v, f->origin, nullptr, nullptr);
        check_annots(&v, &f->annots, nullptr, nullptr);
        check_parsed(&v, f);
        check_defined_once(&v, f);
        check_uses_resolve(&v, f);
    }

    if (!v.bad) {
        held |= IR_PROP_PARSED;

        v.bad = false;
        v.d   = claims(m, IR_PROP_TYPED) ? out : nullptr;

        for (size_t i = 0; i < ir_func_count(m); i++) {
            check_typed(&v, ir_func_at(m, i));
        }

        if (!v.bad) {
            held |= IR_PROP_TYPED;

            v.bad = false;
            v.d   = claims(m, IR_PROP_SSA) ? out : nullptr;

            for (size_t i = 0; i < ir_func_count(m); i++) {
                check_ssa(&v, ir_func_at(m, i));
            }

            if (!v.bad) {
                held |= IR_PROP_SSA;
            }
        }

        v.bad = false;
        v.d   = claims(m, IR_PROP_LAID_OUT) ? out : nullptr;

        for (size_t i = 0; i < ir_func_count(m); i++) {
            check_laid_out(&v, ir_func_at(m, i));
        }

        if (!v.bad) {
            held |= IR_PROP_LAID_OUT;
        }

        v.bad = false;
        v.d   = claims(m, IR_PROP_NO_CRIT_EDGE) ? out : nullptr;

        for (size_t i = 0; i < ir_func_count(m); i++) {
            check_no_critical_edges(&v, ir_func_at(m, i));
        }

        if (!v.bad) {
            held |= IR_PROP_NO_CRIT_EDGE;
        }

        v.bad = false;
        v.d   = claims(m, IR_PROP_STRUCTURED) ? out : nullptr;

        for (size_t i = 0; i < ir_func_count(m); i++) {
            check_structured(&v, ir_func_at(m, i));
        }

        if (!v.bad) {
            held |= IR_PROP_STRUCTURED;
        }
    }
    v.d = out;

    ir_props_t claimed = ir_module_props(m);
    ir_props_t checked = IR_PROP_PARSED | IR_PROP_TYPED | IR_PROP_SSA
                       | IR_PROP_LAID_OUT | IR_PROP_NO_CRIT_EDGE
                       | IR_PROP_STRUCTURED;
    ir_props_t lost    = (ir_props_t)(claimed & checked & ~held);

    for (ir_props_t bit = 1; bit && lost; bit = (ir_props_t)(bit << 1)) {
        if (!(lost & bit)) {
            continue;
        }
        lost = (ir_props_t)(lost & ~bit);

        ir_diag_t *dg = ir_diag_begin(out, IR_DIAG_VERIFY_CLAIM,
                                      ir_module_span(m), "claimed here");

        if (dg) {
            ir_sb sb = {0};

            ir_sb_fmt(&sb, "module %s claims %s, which the verifier finds "
                           "does not hold", m->name, ir_prop_name(bit));

            char *msg = ir_sb_take(&sb);

            ir_diag_message(dg, out, msg);
            free(msg);
        }
    }

    if ((held & IR_PROP_PARSED) && (held & IR_PROP_TYPED)
        && !((claimed & checked) & ~held)) {
        held |= IR_PROP_VERIFIED;
    }
    audit_spans(&v);

    return held;
}

typedef struct {
    ir_span_t    span;
    ir_instr_t  *in;
    ir_block_t  *b;
    const char  *what;
} ir_locrec_t;

struct ir_locsnap_t {
    ir_locrec_t *v;
    size_t       n;
    size_t       cap;
};

static void snap_one(ir_ctx_t *ctx, ir_locsnap_t *s, ir_span_t span,
                     ir_instr_t *in, ir_block_t *b, const char *what)
{
    if (!ir_span_known(span)) {
        return;
    }
    s->v = ir_grow(ctx, s->v, &s->cap, s->n + 1, sizeof(ir_locrec_t));

    if (!s->v) {
        return;
    }
    s->v[s->n++] = (ir_locrec_t){.span = span, .in = in, .b = b, .what = what};
}

ir_locsnap_t *ir_loc_snapshot(ir_module_t *m)
{
    if (!m) {
        return nullptr;
    }

    ir_locsnap_t *s = ir_alloc(m->ctx, sizeof(ir_locsnap_t));

    for (size_t i = 0; i < ir_func_count(m); i++) {
        ir_func_t *f = ir_func_at(m, i);

        snap_one(m->ctx, s, f->span, nullptr, nullptr, "a function");

        for (uint32_t k = 0; k < f->n_locals; k++) {
            snap_one(m->ctx, s, f->locals[k]->span, nullptr, nullptr, "a local");
        }

        for (uint32_t k = 0; k < f->n_blocks; k++) {
            ir_block_t *b = f->blocks[k];

            snap_one(m->ctx, s, b->span, nullptr, b, "a block");

            for (uint32_t j = 0; j < b->n_instrs; j++) {
                snap_one(m->ctx, s, b->instrs[j]->span, b->instrs[j], b,
                         "an instruction");
            }
        }
    }

    return s;
}

bool ir_loc_monotone(ir_locsnap_t *before, ir_module_t *m, const char *agent,
                     ir_diags_t *out)
{
    bool ok = true;

    if (!before || !m) {
        return true;
    }

    size_t live = 1;
    pset   here;

    for (size_t i = 0; i < ir_func_count(m); i++) {
        ir_func_t *f = ir_func_at(m, i);

        for (uint32_t b = 0; b < f->n_blocks; b++) {
            live += f->blocks[b]->n_instrs;
        }
    }
    pset_init(m->ctx, &here, live);

    for (size_t i = 0; i < ir_func_count(m); i++) {
        ir_func_t *f = ir_func_at(m, i);

        for (uint32_t b = 0; b < f->n_blocks; b++) {
            for (uint32_t k = 0; k < f->blocks[b]->n_instrs; k++) {
                pset_add(&here, f->blocks[b]->instrs[k]);
            }
        }
    }

    for (size_t i = 0; i < before->n; i++) {
        ir_locrec_t *r = &before->v[i];

        if (r->in && !pset_has(&here, r->in)) {
            continue;
        }

        ir_span_t now = r->in ? r->in->span : r->b ? r->b->span : r->span;

        if (r->in || r->b) {
            if (ir_span_known(now)) {
                continue;
            }
        }
        else {
            continue;
        }
        ok = false;

        ir_diag_t *dg = ir_diag_begin_at(out, IR_DIAG_LOCATION_LOST, r->in,
                                         r->b, nullptr, "here");

        if (dg) {
            ir_sb sb = {0};

            ir_sb_fmt(&sb, "%s had a known location before %s ran and has "
                           "none after", r->what, agent ? agent : "a pass");

            char *msg = ir_sb_take(&sb);

            ir_diag_message(dg, out, msg);
            free(msg);
        }
    }

    return ok;
}
