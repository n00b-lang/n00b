#include <stdlib.h>

#include "private/ir/internal.h"
#include "ir/rule.h"
#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/text.h"
#include "ir/doms.h"
#include "ir/verify.h"
#include "ir/walk.h"
#include "ir/region_walk.h"

#include "lib/dict.h"

typedef struct {
    ir_module_t *m;
    ir_diags_t  *d;
    bool         bad;
    ir_func_t   *dom_for;
    ir_doms_t   *dom;

    core_dict_t   defined;
    core_dict_t   locals;
    core_dict_t   names;
    core_dict_t   checked;

    uint32_t    *arms;
    uint32_t     terms;
    bool         phis_run;

    ir_walk_t   *collect;
} V;

static bool holds(core_dict_t *s, const void *p)
{
    return p && core_dict_contains(s, (void *)(uintptr_t)p);
}

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

    core_buffer_t *sb = core_buffer_empty();
    va_list ap;

    va_start(ap, fmt);
    core_buffer_vprintf(sb, fmt, ap);
    va_end(ap);

    char *msg = core_buffer_take(sb);

    ir_diag_message(dg, v->d, msg ? msg : rule);
    core_free(msg);
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

static bool collect_param(ir_visit_t *vi)
{
    core_dict_put(&((V *)vi->user)->defined, vi->param, vi->param);

    return true;
}

static bool collect_local(ir_visit_t *vi)
{
    core_dict_put(&((V *)vi->user)->locals, vi->local, vi->local);

    return true;
}

static bool collect_instr(ir_visit_t *vi)
{
    core_dict_put(&((V *)vi->user)->defined, vi->instr, vi->instr);

    return false;
}

static bool parsed_func(ir_visit_t *vi)
{
    V         *v = vi->user;
    ir_func_t *f = vi->func;

    check_origins(v, f->origin, nullptr, nullptr);
    check_annots(v, &f->annots, nullptr, nullptr);

    if (f->n_blocks && ir_pred_count(doms(v, f), ir_block_entry(f))) {
        broke(v, IR_DIAG_VERIFY_PARSED, nullptr, ir_block_entry(f),
              "parsed.entry", "the entry block of %s has a predecessor",
              f->name);
    }

    v->arms = ir_alloc(v->m->ctx, (f->n_blocks + 1) * sizeof(uint32_t));

    core_dict_init(&v->defined, nullptr, nullptr);
    core_dict_init(&v->locals, nullptr, nullptr);
    core_dict_init(&v->names, core_hash_cstring, core_dict_cstr_eq);

    for (uint32_t i = 0; i < f->n_params; i++) {
        core_dict_add(&v->names, f->params[i]->name, f->params[i]);
    }

    ir_walk_run_func(v->collect, f);

    return true;
}

static bool parsed_func_end(ir_visit_t *vi)
{
    V *v = vi->user;

    core_dict_free(&v->defined);
    core_dict_free(&v->locals);
    core_dict_free(&v->names);

    return true;
}

static bool parsed_block(ir_visit_t *vi)
{
    V *v = vi->user;

    check_annots(v, &vi->block->annots, nullptr, vi->block);
    check_origins(v, vi->block->origin, nullptr, vi->block);

    v->terms    = 0;
    v->phis_run = true;

    return true;
}

static bool parsed_block_end(ir_visit_t *vi)
{
    V *v = vi->user;

    if (v->terms != 1) {
        broke(v, IR_DIAG_VERIFY_PARSED, nullptr, vi->block,
              "parsed.terminator",
              "block %s has %u terminators and needs exactly one",
              vi->block->name, v->terms);
    }

    return true;
}

static void check_phi_arms(V *v, ir_instr_t *in, ir_block_t *b)
{
    ir_doms_t *dm = doms(v, b->owner);

    for (uint32_t a = 0; a < in->n_operands; a++) {
        if (in->operands[a].kind == IR_OPD_PHI_ARM
            && in->operands[a].phi.block) {
            v->arms[in->operands[a].phi.block->index]++;
        }
    }

    for (uint32_t p = 0; p < ir_pred_count(dm, b); p++) {
        ir_block_t *pred = ir_pred_at(dm, b, p);

        if (v->arms[pred->index] != 1) {
            broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.phi.arms",
                  "this phi names block %s %u times and %s is a predecessor "
                  "once", pred->name, v->arms[pred->index], pred->name);
        }
        v->arms[pred->index] = 0;
    }

    for (uint32_t a = 0; a < in->n_operands; a++) {
        ir_block_t *arm = in->operands[a].kind == IR_OPD_PHI_ARM
                            ? in->operands[a].phi.block
                            : nullptr;

        if (!arm || !v->arms[arm->index]) {
            continue;
        }
        v->arms[arm->index] = 0;
        broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.phi.arms",
              "this phi names block %s, which is not a predecessor",
              arm->name);
    }
}

static bool parsed_instr(ir_visit_t *vi)
{
    V          *v  = vi->user;
    ir_instr_t *in = vi->instr;
    ir_block_t *b  = vi->block;

    check_annots(v, &in->annots, in, b);
    check_origins(v, in->origin, in, b);

    if (ir_op_is_terminator(in->op)) {
        v->terms++;

        if (in != b->instrs[b->n_instrs - 1]) {
            broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.terminator",
                  "%s terminates block %s but is not its last instruction",
                  ir_module_op_spelling(v->m, in->op), b->name);
        }
    }

    if (is_phi(in) && !v->phis_run) {
        broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.phi.leading",
              "a phi follows a non-phi instruction in block %s", b->name);
    }
    v->phis_run = v->phis_run && is_phi(in);

    const ir_opinfo_t *info = ir_module_op_info(v->m, in->op);

    if (!info) {
        return true;
    }

    if (!ir_opinfo_accepts_operands(info, in->n_operands)) {
        broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.arity",
              "%s takes %u to %u operands and has %u", info->spelling,
              info->min_operands,
              info->max_operands == IR_VAR_ARITY ? info->min_operands
                                                 : info->max_operands,
              in->n_operands);
    }

    for (uint32_t o = 0; o < in->n_mods; o++) {
        if (!ir_opinfo_accepts_mod(info, in->mods[o])) {
            broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.modifier",
                  "%s does not take the modifier %s", info->spelling,
                  in->mods[o]);
        }
    }

    return true;
}

static bool parsed_operand(ir_visit_t *vi)
{
    V                 *v    = vi->user;
    ir_instr_t        *in   = vi->instr;
    ir_block_t        *b    = vi->block;
    ir_operand_t      *od   = vi->operand;
    const ir_opinfo_t *info = ir_module_op_info(v->m, in->op);

    if (info) {
        ir_opdkind_t want = ir_opinfo_operand_kind(info, vi->index);
        ir_opdkind_t got  = od->kind;
        bool         fits = got == want
                         || (want == IR_OPD_ARG
                             && (got == IR_OPD_VALUE
                                 || got == IR_OPD_SPREAD
                                 || got == IR_OPD_NAMED_ARG));

        if (!fits) {
            broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.operand",
                  "operand %u of %s is a %s where the table says %s",
                  vi->index, info->spelling, ir_opdkind_name(got),
                  ir_opdkind_name(want));
        }

        ir_block_t *to = od->kind == IR_OPD_BLOCK         ? od->block
                       : od->kind == IR_OPD_DEFAULT_BLOCK ? od->block
                       : od->kind == IR_OPD_SWITCH_ARM    ? od->arm.block
                       : od->kind == IR_OPD_PHI_ARM       ? od->phi.block
                                                          : nullptr;

        if (to && !names_block(b->owner, to)) {
            broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.block-ref",
                  "block %s is not a block of %s", to->name, b->owner->name);
        }
    }

    ir_value_t *val = od->kind == IR_OPD_VALUE     ? od->value
                    : od->kind == IR_OPD_SPREAD   ? od->value
                    : od->kind == IR_OPD_NAMED_ARG ? od->named.value
                    : od->kind == IR_OPD_PHI_ARM   ? od->phi.value
                                                   : nullptr;

    if (!val) {
        return true;
    }

    bool ok = true;

    switch (val->kind) {
    case IR_VAL_GLOBAL:
    case IR_VAL_FUNC:
        break;
    case IR_VAL_LOCAL:
        ok = holds(&v->locals, val->local);
        break;
    case IR_VAL_PARAM:
        ok = holds(&v->defined, val);
        break;
    default:
        ok = holds(&v->defined, val->def);
        break;
    }

    if (!ok) {
        broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.value-ref",
              "%%%s is not a parameter, local or instruction of %s",
              val->name ? val->name : "?", b->owner->name);
    }

    return true;
}

static bool parsed_instr_end(ir_visit_t *vi)
{
    V          *v  = vi->user;
    ir_instr_t *in = vi->instr;
    ir_block_t *b  = vi->block;

    if (in->dest_kind == IR_DEST_VALUE && in->dest
        && !core_dict_add(&v->names, in->dest->name, in)) {
        broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.one-def",
              "%%%s is defined more than once in %s, or shadows a parameter",
              in->dest->name, b->owner->name);
    }

    if (is_phi(in) && ir_module_op_info(v->m, in->op)) {
        check_phi_arms(v, in, b);
    }

    return true;
}

static bool typed_param(ir_visit_t *vi)
{
    V *v = vi->user;

    if (!vi->param->type) {
        broke(v, IR_DIAG_VERIFY_TYPED, nullptr, ir_block_entry(vi->func),
              "typed.value", "parameter %s of %s has no type",
              vi->param->name, vi->func->name);
    }

    return true;
}

static bool typed_instr(ir_visit_t *vi)
{
    V *v = vi->user;

    if (ir_module_op_info(v->m, vi->instr->op)) {
        return true;
    }

    broke(v, IR_DIAG_MISSING_PROFILE, vi->instr, vi->block, "typed.extension",
          "this extension opcode is not one profile %s supplies",
          v->m->profile ? v->m->profile->name : "(none)");

    return false;
}

static bool typed_operand(ir_visit_t *vi)
{
    V          *v   = vi->user;
    ir_value_t *val = vi->operand->kind == IR_OPD_VALUE ||
                      vi->operand->kind == IR_OPD_SPREAD
                          ? vi->operand->value : nullptr;

    if (val && !val->type) {
        broke(v, IR_DIAG_VERIFY_TYPED, vi->instr, vi->block, "typed.value",
              "operand %u has no type", vi->index);
    }

    return true;
}

static bool typed_instr_end(ir_visit_t *vi)
{
    V          *v  = vi->user;
    ir_instr_t *in = vi->instr;

    if (!ir_module_op_info(v->m, in->op)) {
        return true;
    }

    ir_verdict_t verdict = ir_rule_apply(v->m, ir_module_rules(v->m), in,
                                         v->d).verdict;

    if (verdict == IR_RULE_DECLINE) {
        broke(v, IR_DIAG_VERIFY_TYPED, in, vi->block, "typed.rule",
              "no typing rule resolves for %s with these operand types",
              ir_module_op_spelling(v->m, in->op));
    }
    else if (verdict != IR_RULE_ACCEPT) {
        v->bad = true;
    }

    return true;
}

static bool ssa_func(ir_visit_t *vi)
{
    V *v = vi->user;

    if (vi->func->n_locals) {
        broke(v, IR_DIAG_VERIFY_SSA, nullptr, ir_block_entry(vi->func),
              "ssa.locals", "%s is in SSA form and still declares %u locals",
              vi->func->name, vi->func->n_locals);
    }

    return true;
}

static bool ssa_instr(ir_visit_t *vi)
{
    V *v = vi->user;

    if (vi->instr->dest_kind == IR_DEST_LOCAL) {
        broke(v, IR_DIAG_VERIFY_SSA, vi->instr, vi->block, "ssa.locals",
              "this instruction writes local $%s in SSA form",
              vi->instr->dest_local ? vi->instr->dest_local->name : "?");
    }

    return true;
}

static bool ssa_operand(ir_visit_t *vi)
{
    V            *v   = vi->user;
    ir_instr_t   *in  = vi->instr;
    ir_block_t   *b   = vi->block;
    ir_operand_t *od  = vi->operand;
    ir_value_t   *val = od->kind == IR_OPD_VALUE     ? od->value
                      : od->kind == IR_OPD_SPREAD   ? od->value
                      : od->kind == IR_OPD_NAMED_ARG ? od->named.value
                      : od->kind == IR_OPD_PHI_ARM   ? od->phi.value
                                                     : nullptr;

    if (!val || val->kind != IR_VAL_INSTR || !val->def || !val->def->block) {
        return true;
    }

    ir_doms_t  *dom  = doms(v, b->owner);
    ir_block_t *from = od->kind == IR_OPD_PHI_ARM ? od->phi.block : b;

    if (!ir_block_reachable(dom, b)
        || (od->kind == IR_OPD_PHI_ARM
            && !ir_block_reachable(dom, from))) {
        return true;
    }

    if (!ir_dominates(dom, val->def->block, from)) {
        broke(v, IR_DIAG_VERIFY_SSA, in, b, "ssa.dominance",
              "%%%s is used here but its definition in block %s does not "
              "dominate %s", val->name, val->def->block->name, from->name);

        return true;
    }

    if (val->def->block == from && od->kind != IR_OPD_PHI_ARM
        && val->def->index >= in->index) {
        broke(v, IR_DIAG_VERIFY_SSA, in, b, "ssa.dominance",
              "%%%s is used before it is defined in block %s", val->name,
              b->name);
    }

    return true;
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

    if (!core_dict_add(&v->checked, t, t)) {
        return;
    }

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

static bool laid_out_param(ir_visit_t *vi)
{
    check_laid_out_type(vi->user, vi->param->type, nullptr,
                        ir_block_entry(vi->func));

    return true;
}

static bool laid_out_local(ir_visit_t *vi)
{
    check_laid_out_type(vi->user, vi->local->type, nullptr,
                        ir_block_entry(vi->func));

    return true;
}

static bool laid_out_instr(ir_visit_t *vi)
{
    ir_instr_t *in = vi->instr;

    if (in->dest_kind == IR_DEST_VALUE && in->dest) {
        check_laid_out_type(vi->user, in->dest->type, in, vi->block);
    }

    return true;
}

static bool laid_out_operand(ir_visit_t *vi)
{
    ir_operand_t *od = vi->operand;

    if (od->kind == IR_OPD_TYPE) {
        check_laid_out_type(vi->user, od->type, vi->instr, vi->block);
    }

    if ((od->kind == IR_OPD_VALUE || od->kind == IR_OPD_SPREAD) && od->value) {
        check_laid_out_type(vi->user, od->value->type, vi->instr, vi->block);
    }

    return true;
}

static bool laid_out_bitcast(ir_visit_t *vi)
{
    V          *v  = vi->user;
    ir_instr_t *in = vi->instr;

    if (!in->n_operands || in->operands[0].kind != IR_OPD_VALUE
        || !in->operands[0].value || !in->dest) {
        return true;
    }

    uint64_t from = 0;
    uint64_t to   = 0;

    if (settled_for(v, in->operands[0].value->type, &from)
        && settled_for(v, in->dest->type, &to) && from != to) {
        broke(v, IR_DIAG_VERIFY_LAID_OUT, in, vi->block, "laid-out.bitcast",
              "bitcast turns %llu bytes of %s into %llu bytes of %s",
              (unsigned long long)from,
              ir_type_text(v->m, in->operands[0].value->type),
              (unsigned long long)to, ir_type_text(v->m, in->dest->type));
    }

    return true;
}

static bool no_critical_edges(ir_visit_t *vi)
{
    V          *v = vi->user;
    ir_block_t *b = vi->block;
    ir_instr_t *t = ir_block_terminator(b);

    if (ir_succ_count(t) < 2) {
        return false;
    }

    ir_doms_t *dom = doms(v, b->owner);

    for (uint32_t s = 0; s < t->n_operands; s++) {
        ir_block_t *to = ir_succ_of(&t->operands[s]);

        if (to && ir_pred_count(dom, to) > 1) {
            broke(v, IR_DIAG_VERIFY_EDGE, t, b, "no-critical-edges.edge",
                  "the edge from %s to %s is critical: %s has several "
                  "successors and %s several predecessors", b->name, to->name,
                  b->name, to->name);
        }
    }

    return false;
}

typedef struct {
    uint32_t *seen;
    uint32_t  n;
} tally_t;

static bool tally_block(ir_region_visit_t *v)
{
    tally_t *t = v->user;

    if (v->region->head && v->region->head->index < t->n) {
        t->seen[v->region->head->index]++;
    }

    return true;
}

static void tally(ir_region_t *r, uint32_t *seen, uint32_t n)
{
    tally_t t = {.seen = seen, .n = n};

    ir_region_walk_t *w = ir_region_walk_new(&t);

    ir_region_on_kind(w, IR_REGION_BLOCK, tally_block, nullptr, nullptr);
    ir_region_walk_run(w, r);
    ir_region_walk_free(w);
}

static bool check_structured(ir_visit_t *vi)
{
    V           *v    = vi->user;
    ir_func_t   *f    = vi->func;
    ir_region_t *root = ir_region_root(f);

    if (!root) {
        broke(v, IR_DIAG_VERIFY_STRUCTURE, nullptr, ir_block_entry(f),
              "structured.overlay", "%s has no region overlay", f->name);

        return false;
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

    return false;
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
                            "which is a bug in whatever reported it");
        }

        return;
    }
}

static bool parsed_module(ir_visit_t *vi)
{
    check_annots(vi->user, ir_module_annots(vi->module), nullptr, nullptr);

    return true;
}

static void install_parsed(ir_walk_t *w)
{
    ir_walk_on_module(w, parsed_module, nullptr, nullptr);
    ir_walk_on_func(w, parsed_func, parsed_func_end, nullptr);
    ir_walk_on_block(w, parsed_block, parsed_block_end, nullptr);
    ir_walk_on_instr(w, parsed_instr, parsed_instr_end, nullptr);
    ir_walk_on_operand(w, parsed_operand, nullptr);
}

static void install_typed(ir_walk_t *w)
{
    ir_walk_on_param(w, typed_param, nullptr);
    ir_walk_on_instr(w, typed_instr, typed_instr_end, nullptr);
    ir_walk_on_operand(w, typed_operand, nullptr);
}

static void install_ssa(ir_walk_t *w)
{
    ir_walk_on_func(w, ssa_func, nullptr, nullptr);
    ir_walk_on_instr(w, ssa_instr, nullptr, nullptr);
    ir_walk_on_operand(w, ssa_operand, nullptr);
}

static void install_laid_out(ir_walk_t *w)
{
    ir_walk_on_param(w, laid_out_param, nullptr);
    ir_walk_on_local(w, laid_out_local, nullptr);
    ir_walk_on_instr(w, laid_out_instr, nullptr, nullptr);
    ir_walk_on_operand(w, laid_out_operand, nullptr);
    ir_walk_on_op(w, IR_OP_BITCAST, nullptr, laid_out_bitcast, nullptr);
}

static void install_no_crit_edge(ir_walk_t *w)
{
    ir_walk_on_block(w, no_critical_edges, nullptr, nullptr);
}

static void install_structured(ir_walk_t *w)
{
    ir_walk_on_func(w, check_structured, nullptr, nullptr);
}

typedef struct {
    ir_props_t prop;
    ir_props_t needs;
    void     (*install)(ir_walk_t *w);
} phase_t;

static const phase_t PHASES[] = {
    {IR_PROP_PARSED,       0,              install_parsed       },
    {IR_PROP_TYPED,        IR_PROP_PARSED, install_typed        },
    {IR_PROP_SSA,          IR_PROP_TYPED,  install_ssa          },
    {IR_PROP_LAID_OUT,     IR_PROP_PARSED, install_laid_out     },
    {IR_PROP_NO_CRIT_EDGE, IR_PROP_PARSED, install_no_crit_edge },
    {IR_PROP_STRUCTURED,   IR_PROP_PARSED, install_structured   },
};

#define N_PHASES (sizeof(PHASES) / sizeof(PHASES[0]))

static ir_props_t with_needs(ir_props_t want)
{
    for (size_t i = N_PHASES; i-- > 0;) {
        if (want & PHASES[i].prop) {
            want |= PHASES[i].needs;
        }
    }

    return want;
}

static ir_props_t run_phases(ir_module_t *m, ir_props_t want, ir_diags_t *out,
                             bool only_when_claimed)
{
    V          v    = {.m = m, .d = out};
    ir_props_t run  = with_needs(want);
    ir_props_t held = 0;

    core_dict_init(&v.checked, nullptr, nullptr);

    v.collect = ir_walk_new(&v);

    ir_walk_on_param(v.collect, collect_param, nullptr);
    ir_walk_on_local(v.collect, collect_local, nullptr);
    ir_walk_on_instr(v.collect, collect_instr, nullptr, nullptr);

    for (size_t i = 0; i < N_PHASES; i++) {
        const phase_t *p = &PHASES[i];

        if (!(run & p->prop) || (held & p->needs) != p->needs) {
            continue;
        }

        v.bad = false;
        v.d   = only_when_claimed && !claims(m, p->prop) ? nullptr : out;

        ir_walk_t *w = ir_walk_new(&v);

        p->install(w);
        ir_walk_run(w, m);
        ir_walk_free(w);

        if (!v.bad) {
            held |= p->prop;
        }
    }
    v.d = out;

    audit_spans(&v);

    core_dict_free(&v.checked);
    ir_walk_free(v.collect);

    return held;
}

ir_props_t ir_verify_only(ir_module_t *m, ir_props_t want, ir_diags_t *out)
{
    return m ? run_phases(m, want, out, false) : 0;
}

ir_props_t ir_verify(ir_module_t *m, ir_diags_t *out)
{
    if (!m) {
        return 0;
    }

    ir_props_t checked = 0;

    for (size_t i = 0; i < N_PHASES; i++) {
        checked |= PHASES[i].prop;
    }

    ir_props_t held    = run_phases(m, checked, out, true);
    ir_props_t claimed = ir_module_props(m);
    ir_props_t lost    = (ir_props_t)(claimed & checked & ~held);

    for (ir_props_t bit = 1; bit && lost; bit = (ir_props_t)(bit << 1)) {
        if (!(lost & bit)) {
            continue;
        }
        lost = (ir_props_t)(lost & ~bit);

        ir_diag_t *dg = ir_diag_begin(out, IR_DIAG_VERIFY_CLAIM,
                                      ir_module_span(m), "claimed here");

        if (dg) {
            core_buffer_t *sb = core_buffer_empty();

            core_buffer_printf(sb, "module %s claims %s, which the verifier finds "
                           "does not hold", m->name, ir_prop_name(bit));

            char *msg = core_buffer_take(sb);

            ir_diag_message(dg, out, msg);
            core_free(msg);
        }
    }

    if ((held & IR_PROP_PARSED) && (held & IR_PROP_TYPED)
        && !((claimed & checked) & ~held)) {
        held |= IR_PROP_VERIFIED;
    }

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

static bool live_instr(ir_visit_t *vi)
{
    core_dict_put((core_dict_t *)vi->user, vi->instr, vi->instr);

    return false;
}

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

static bool snap_func(ir_visit_t *vi)
{
    snap_one(vi->module->ctx, vi->user, vi->func->span, nullptr, nullptr,
             "a function");

    return true;
}

static bool snap_local(ir_visit_t *vi)
{
    snap_one(vi->module->ctx, vi->user, vi->local->span, nullptr, nullptr,
             "a local");

    return true;
}

static bool snap_block(ir_visit_t *vi)
{
    snap_one(vi->module->ctx, vi->user, vi->block->span, nullptr, vi->block,
             "a block");

    return true;
}

static bool snap_instr(ir_visit_t *vi)
{
    snap_one(vi->module->ctx, vi->user, vi->instr->span, vi->instr, vi->block,
             "an instruction");

    return false;
}

ir_locsnap_t *ir_loc_snapshot(ir_module_t *m)
{
    if (!m) {
        return nullptr;
    }

    ir_locsnap_t *s = ir_alloc(m->ctx, sizeof(ir_locsnap_t));
    ir_walk_t    *w = ir_walk_new(s);

    ir_walk_on_func(w, snap_func, nullptr, nullptr);
    ir_walk_on_local(w, snap_local, nullptr);
    ir_walk_on_block(w, snap_block, nullptr, nullptr);
    ir_walk_on_instr(w, snap_instr, nullptr, nullptr);
    ir_walk_run(w, m);
    ir_walk_free(w);

    return s;
}

bool ir_loc_monotone(ir_locsnap_t *before, ir_module_t *m, const char *agent,
                     ir_diags_t *out)
{
    bool ok = true;

    if (!before || !m) {
        return true;
    }

    core_dict_t here;

    core_dict_init(&here, nullptr, nullptr);

    ir_walk_t *w = ir_walk_new(&here);

    ir_walk_on_instr(w, live_instr, nullptr, nullptr);
    ir_walk_run(w, m);
    ir_walk_free(w);

    for (size_t i = 0; i < before->n; i++) {
        ir_locrec_t *r = &before->v[i];

        if (r->in && !holds(&here, r->in)) {
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
            core_buffer_t *sb = core_buffer_empty();

            core_buffer_printf(sb, "%s had a known location before %s ran and has "
                           "none after", r->what, agent ? agent : "a pass");

            char *msg = core_buffer_take(sb);

            ir_diag_message(dg, out, msg);
            core_free(msg);
        }
    }
    core_dict_free(&here);

    return ok;
}
