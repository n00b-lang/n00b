#include <stdlib.h>
#include <stdio.h>

#include "private/ir/internal.h"
#include "ir/rule.h"
#include "ir/layout.h"
#include "ir/pass.h"
#include "ir/text.h"
#include "ir/typelang.h"
#include "ir/doms.h"
#include "ir/verify.h"
#include "ir/walk.h"
#include "ir/region_walk.h"

#include "lib/dict.h"
#include "lib/list.h"

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

#define SARG(s) (int)(s).u8_bytes, (s).data

__attribute__((format(printf, 6, 7))) static void
broke_at(V *v, ir_diagcode_t code, ir_instr_t *in, ir_block_t *b,
         core_string_t rule, const char *fmt, ...)
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

    core_string_t msg = core_buffer_take_string(sb);

    ir_diag_message(dg, v->d, msg);
    core_free(msg.data);
}

#define broke(v, code, in, b, rule, ...) \
    broke_at((v), (code), (in), (b), CORE_STRING_STATIC(rule), __VA_ARGS__)

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
        if (o->kind != IR_ORIGIN_SOURCE && !o->agent.data) {
            broke(v, IR_DIAG_VERIFY_ORIGIN, in, b, "origin.agent",
                  "an origin of kind %.*s names no agent",
                  SARG(ir_origin_kind_name(o->kind)));
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
        core_string_t key = ir_annot_at(a, i)->key;

        if (!ir_annot_key_ok(key, v->m->profile ? v->m->profile->name
                                                : CORE_STRING_STATIC(""))) {
            broke(v, IR_DIAG_BAD_ANNOT_KEY, in, b, "annot.namespace",
                  "the annotation key %.*s is not namespaced by ir or by the "
                  "profile", SARG(key));
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
              "parsed.entry", "the entry block of %.*s has a predecessor",
              SARG(f->name));
    }

    v->arms = ir_alloc(v->m->ctx, (f->n_blocks + 1) * sizeof(uint32_t));

    core_dict_init(&v->defined, nullptr, nullptr);
    core_dict_init(&v->locals, nullptr, nullptr);
    core_dict_init_string(&v->names);

    for (uint32_t i = 0; i < f->n_params; i++) {
        core_dict_add(&v->names, &f->params[i]->name, f->params[i]);
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
              "block %.*s has %u terminators and needs exactly one",
              SARG(vi->block->name), v->terms);
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
                  "this phi names block %.*s %u times and %.*s is a "
                  "predecessor once", SARG(pred->name), v->arms[pred->index],
                  SARG(pred->name));
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
              "this phi names block %.*s, which is not a predecessor",
              SARG(arm->name));
    }
}

static bool parsed_instr(ir_visit_t *vi)
{
    V          *v  = vi->user;
    ir_instr_t *in = vi->instr;
    ir_block_t *b  = vi->block;

    check_annots(v, &in->annots, in, b);
    check_origins(v, in->origin, in, b);
    if (in->op == IR_OP_PHASE_RESULT
        && !ir_instr_phase_result(in).u8_bytes) {
        broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.phase-result-id",
              "phase.result requires a nonempty ir.phase_result identifier");
    }

    if (ir_op_is_terminator(in->op)) {
        v->terms++;

        if (in != b->instrs[b->n_instrs - 1]) {
            broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.terminator",
                  "%.*s terminates block %.*s but is not its last instruction",
                  SARG(ir_module_op_spelling(v->m, in->op)), SARG(b->name));
        }
    }

    if (is_phi(in) && !v->phis_run) {
        broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.phi.leading",
              "a phi follows a non-phi instruction in block %.*s",
              SARG(b->name));
    }
    v->phis_run = v->phis_run && is_phi(in);

    const ir_opinfo_t *info = ir_module_op_info(v->m, in->op);

    if (!info) {
        return true;
    }

    if (!ir_opinfo_accepts_operands(info, in->n_operands)) {
        broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.arity",
              "%.*s takes %u to %u operands and has %u",
              SARG(info->spelling),
              info->min_operands,
              info->max_operands == IR_VAR_ARITY ? info->min_operands
                                                 : info->max_operands,
              in->n_operands);
    }

    for (uint32_t o = 0; o < in->n_mods; o++) {
        if (!ir_opinfo_accepts_mod(info, in->mods[o])) {
            broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.modifier",
                  "%.*s does not take the modifier %.*s",
                  SARG(info->spelling), SARG(in->mods[o]));
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
                                 || got == IR_OPD_NAMED_ARG
                                 || got == IR_OPD_OUT
                                 || got == IR_OPD_FRESH
                                 || got == IR_OPD_MISSING));

        if (!fits) {
            broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.operand",
                  "operand %u of %.*s is a %.*s where the table says %.*s",
                  vi->index, SARG(info->spelling),
                  SARG(ir_opdkind_name(got)), SARG(ir_opdkind_name(want)));
        }

        ir_block_t *to = od->kind == IR_OPD_BLOCK         ? od->block
                       : od->kind == IR_OPD_DEFAULT_BLOCK ? od->block
                       : od->kind == IR_OPD_SWITCH_ARM    ? od->arm.block
                       : od->kind == IR_OPD_PHI_ARM       ? od->phi.block
                                                          : nullptr;

        if (to && !names_block(b->owner, to)) {
            broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.block-ref",
                  "block %.*s is not a block of %.*s", SARG(to->name),
                  SARG(b->owner->name));
        }
    }

    ir_value_t *val = od->kind == IR_OPD_VALUE     ? od->value
                    : od->kind == IR_OPD_SPREAD   ? od->value
                    : od->kind == IR_OPD_FRESH    ? od->value
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
              "%%%.*s is not a parameter, local or instruction of %.*s",
              SARG(val->name.data ? val->name : CORE_STRING_STATIC("?")),
              SARG(b->owner->name));
    }

    return true;
}

static bool parsed_instr_end(ir_visit_t *vi)
{
    V          *v  = vi->user;
    ir_instr_t *in = vi->instr;
    ir_block_t *b  = vi->block;

    if (in->dest_kind == IR_DEST_VALUE && in->dest
        && !core_dict_add(&v->names, &in->dest->name, in)) {
        broke(v, IR_DIAG_VERIFY_PARSED, in, b, "parsed.one-def",
              "%%%.*s is defined more than once in %.*s, or shadows a "
              "parameter", SARG(in->dest->name), SARG(b->owner->name));
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
              "typed.value", "parameter %.*s of %.*s has no type",
              SARG(vi->param->name), SARG(vi->func->name));
    }

    return true;
}

static bool typed_global(ir_visit_t *vi)
{
    V *v = vi->user;
    if (vi->global->has_static_initializer
        && !ir_global_static_initializer_valid(v->m, vi->global)) {
        v->bad = true;
        if (v->d) {
            ir_diag_t *diagnostic = ir_diag_begin(v->d,
                IR_DIAG_VERIFY_TYPED, vi->global->span,
                CORE_STRING_STATIC("this global static initializer"));
            if (diagnostic) {
                core_buffer_t *sb = core_buffer_empty();
                core_buffer_printf(sb,
                    "static initializer for global %.*s does not match its declared type",
                    SARG(vi->global->name));
                core_string_t message = core_buffer_take_string(sb);
                ir_diag_message(diagnostic, v->d,
                                core_string_slice(message, 0, 255));
                core_free(message.data);
            }
        }
    }
    ir_annot_t *requirement = ir_annot_get(
        (ir_annots_t *)&vi->global->annots,
        CORE_STRING_STATIC("ir.storage_required"));
    if (!requirement) return true;

    bool valid_requirement = requirement->kind == IR_ANNOT_BOOL;
    bool required = !valid_requirement || requirement->b;
    if (!required) return true;

    bool reported_required = false;
    core_string_t requested = {};
    core_string_t selected = ir_global_storage_class(
        vi->global, &reported_required, &requested);
    if (valid_requirement && reported_required && requested.data
        && selected.data && core_string_eq(requested, selected)) return true;

    v->bad = true;
    if (v->d) {
        ir_diag_t *diagnostic = ir_diag_begin(v->d,
            IR_DIAG_VERIFY_STORAGE, vi->global->span,
            CORE_STRING_STATIC("this global storage requirement"));
        if (diagnostic) {
            core_buffer_t *sb = core_buffer_empty();
            core_buffer_printf(sb,
                "global %.*s requires storage class %.*s, but selected class is %.*s",
                SARG(vi->global->name),
                SARG(valid_requirement && requested.data
                         ? requested : CORE_STRING_STATIC("(missing)")),
                SARG(selected.data ? selected
                                   : CORE_STRING_STATIC("(missing)")));
            core_string_t message = core_buffer_take_string(sb);
            ir_diag_message(diagnostic, v->d,
                            core_string_slice(message, 0, 319));
            core_free(message.data);
        }
    }
    return true;
}

static bool typed_allocation_storage(ir_visit_t *vi)
{
    ir_instr_t *allocation = vi->instr;
    if (allocation->op != IR_OP_ALLOC) return true;

    ir_annot_t *required = ir_annot_get(
        &allocation->annots, CORE_STRING_STATIC("ir.storage_required"));
    if (!required) return true;
    if (required->kind != IR_ANNOT_BOOL) {
        broke(vi->user, IR_DIAG_VERIFY_STORAGE, allocation, vi->block,
              "storage.required",
              "allocation has malformed required storage metadata");
        return false;
    }
    if (!required->b) return true;

    ir_annot_t *storage = ir_annot_get(
        &allocation->annots, CORE_STRING_STATIC("ir.storage_class"));
    if (storage && storage->kind == IR_ANNOT_STR && storage->s.u8_bytes)
        return true;

    broke(vi->user, IR_DIAG_VERIFY_STORAGE, allocation, vi->block,
          "storage.required",
          "allocation requires a storage class, but its selected class is %s",
          storage && storage->kind == IR_ANNOT_STR ? "empty" : "missing");
    return false;
}

typedef ir_value_t *verify_value_ptr_t;
typedef ir_instr_t *verify_instr_ptr_t;
core_list_decl(verify_value_ptr_t);
core_list_decl(verify_instr_ptr_t);

typedef struct {
    core_list_t(verify_value_ptr_t) pending;
    core_list_t(verify_value_ptr_t) targets;
    core_list_t(verify_value_ptr_t) containers;
    th_ctx *types;
    bool external_writable;
    bool writable_projection;
    bool contents;
    bool complete;
} store_target_set_t;

static bool store_target_add(core_list_t(verify_value_ptr_t) *items,
                             ir_value_t *value)
{
    return core_option_is_set(core_list_find(*items, value))
        || core_list_try_push(*items, value);
}

static bool store_target_enqueue(store_target_set_t *set, ir_value_t *value)
{
    return value && store_target_add(&set->pending, value);
}

static bool store_target_carries_reference(const store_target_set_t *set,
                                           th_type *type)
{
    if (!type) return false;
    type = th_resolve(type);
    if (th_kind_of(type) == TH_INDIRECTION) return true;
    th_type *definition = set->types && th_kind_of(type) == TH_NAMED
        ? th_type_definition(set->types, type) : nullptr;
    if (definition && th_kind_of(th_resolve(definition)) == TH_INDIRECTION)
        return true;
    th_type *item = th_option_item(type);
    return item && store_target_carries_reference(set, item);
}

static bool store_target_enqueue_indirect(store_target_set_t *set,
                                          ir_value_t *value)
{
    return value && value->type
        && store_target_carries_reference(set, value->type)
        && store_target_enqueue(set, value);
}

static bool store_target_finish(store_target_set_t *set, ir_value_t *value)
{
    return value && store_target_add(&set->targets, value);
}

static bool store_target_reference_is_writable(const ir_value_t *value);

static bool store_target_annotation_edges(store_target_set_t *set,
                                          ir_value_t *value)
{
    ir_annots_t *annots = ir_value_annots(value);
    ir_annot_t *count = ir_annot_get(annots,
                                     CORE_STRING_STATIC("ir.alias_count"));
    if (!count || count->kind != IR_ANNOT_INT || count->i.neg
        || count->i.mag > UINT32_MAX
        || count->i.mag > ir_annot_count(annots)) return false;
    ir_annot_t *resolved = ir_annot_get(
        annots, CORE_STRING_STATIC("ir.alias_analysis_complete"));
    ir_annot_t *unresolved = ir_annot_get(
        annots, CORE_STRING_STATIC("ir.alias_unresolved"));
    if (!resolved || resolved->kind != IR_ANNOT_BOOL
        || !unresolved || unresolved->kind != IR_ANNOT_BOOL
        || (resolved->b && unresolved->b)
        || (!resolved->b && !unresolved->b)
        || (!resolved->b
            && !store_target_reference_is_writable(value)
            && !set->writable_projection)) return false;

    for (uint64_t i = 0; i < count->i.mag; i++) {
        char key[48];
        int length = snprintf(key, sizeof(key), "ir.alias%llu",
                              (unsigned long long)i);
        ir_annot_t *edge = length > 0
            ? ir_annot_get(annots, core_string_borrow(key, (size_t)length))
            : nullptr;
        if (!edge || edge->kind != IR_ANNOT_VALUE || !edge->v
            || !store_target_enqueue(set, edge->v)) return false;
    }
    if (!resolved->b) set->external_writable = true;
    return true;
}

static bool store_target_reference_is_writable(const ir_value_t *value)
{
    ir_indirect_access_t access = ir_value_indirect_access(value);
    return access == IR_INDIRECT_ACCESS_WRITE
        || access == IR_INDIRECT_ACCESS_READ_WRITE;
}

static bool store_target_root_has_no_known_aliases(
    const ir_value_t *value)
{
    ir_annots_t *annots = ir_value_annots((ir_value_t *)value);
    ir_annot_t *count = annots
        ? ir_annot_get(annots, CORE_STRING_STATIC("ir.alias_count"))
        : nullptr;
    ir_annot_t *complete = annots
        ? ir_annot_get(annots,
                       CORE_STRING_STATIC("ir.alias_analysis_complete"))
        : nullptr;
    return value
        && (value->kind == IR_VAL_PARAM || value->kind == IR_VAL_GLOBAL)
        && count && count->kind == IR_ANNOT_INT && !count->i.neg
        && !count->i.mag
        && complete && complete->kind == IR_ANNOT_BOOL;
}

bool ir_value_addresses_sequence_contents(const ir_value_t *address)
{
    const ir_instr_t *definition = address && address->kind == IR_VAL_INSTR
        ? address->def : nullptr;
    ir_value_t *container = definition && definition->op == IR_OP_INDEX
        && definition->n_operands
        ? ir_operand_value(&definition->operands[0]) : nullptr;
    th_type *type = container && container->type
        ? th_resolve(container->type) : nullptr;
    th_type *referent = type && th_kind_of(type) == TH_INDIRECTION
        ? th_ind_referent(type) : nullptr;
    return referent && th_kind_of(th_resolve(referent)) == TH_SEQUENCE;
}

static bool store_target_collect(store_target_set_t *set,
                                 ir_value_t *address,
                                 bool preserve_parameters,
                                 bool through_contents)
{
    memset(set, 0, sizeof(*set));
    set->types = address && address->owner && address->owner->owner
        ? ir_module_types(address->owner->owner) : nullptr;
    set->complete = store_target_enqueue(set, address);
    for (size_t cursor = 0; set->complete && cursor < set->pending.len;
         cursor++) {
        ir_value_t *value = set->pending.data[cursor];
        if (ir_value_has_external_writable_storage(value)) {
            set->external_writable = true;
            continue;
        }
        if (value->kind != IR_VAL_INSTR || !value->def) {
            if (store_target_carries_reference(set, value->type)) {
                if (preserve_parameters && value->kind == IR_VAL_PARAM) {
                    set->complete = store_target_finish(set, value);
                    continue;
                }
                ir_annot_t *aliases = ir_annot_get(
                    ir_value_annots(value),
                    CORE_STRING_STATIC("ir.alias_count"));
                if (aliases && store_target_root_has_no_known_aliases(
                        value) && (store_target_reference_is_writable(value)
                                  || set->writable_projection))
                    set->external_writable = true;
                else if (aliases)
                    set->complete = store_target_annotation_edges(set, value);
                else if (store_target_reference_is_writable(value)
                         || set->writable_projection)
                    set->external_writable = true;
                else
                    set->complete = false;
                continue;
            }
            set->complete = store_target_finish(set, value);
            continue;
        }

        ir_instr_t *definition = value->def;
        if (!through_contents && ir_value_addresses_sequence_contents(value)) {
            set->contents = true;
            set->complete = store_target_add(&set->containers,
                ir_operand_value(&definition->operands[0]));
            continue;
        }
        switch (definition->op) {
        case IR_OP_ALLOC:
        case IR_OP_ALLOC_USING:
        case IR_OP_ALLOCA:
            set->complete = store_target_finish(set, value);
            break;
        case IR_OP_ADDRESS_OF:
            set->complete = definition->n_operands == 1
                && store_target_finish(set,
                    ir_operand_value(&definition->operands[0]));
            break;
        case IR_OP_COPY:
        case IR_OP_CAST:
        case IR_OP_BITCAST:
        case IR_OP_NOMINAL_CAST:
        case IR_OP_FIELD:
        case IR_OP_INDEX:
        case IR_OP_OFFSET:
        case IR_OP_OPTION_SOME:
            set->complete = definition->n_operands > 0
                && store_target_enqueue_indirect(set,
                    ir_operand_value(&definition->operands[0]));
            break;
        case IR_OP_OPTION_GET:
            set->writable_projection |= store_target_reference_is_writable(value);
            set->complete = definition->n_operands == 1
                && store_target_enqueue(set,
                    ir_operand_value(&definition->operands[0]));
            break;
        case IR_OP_OPTION_GET_OR_ELSE:
            set->writable_projection |= store_target_reference_is_writable(value);
            set->complete = definition->n_operands == 2
                && store_target_enqueue(set,
                    ir_operand_value(&definition->operands[0]))
                && store_target_enqueue_indirect(set,
                    ir_operand_value(&definition->operands[1]));
            break;
        case IR_OP_SELECT:
            set->complete = definition->n_operands == 3
                && store_target_enqueue_indirect(set,
                    ir_operand_value(&definition->operands[1]))
                && store_target_enqueue_indirect(set,
                    ir_operand_value(&definition->operands[2]));
            break;
        case IR_OP_PHI:
            if (!definition->n_operands) set->complete = false;
            for (uint32_t i = 0; set->complete
                 && i < definition->n_operands; i++)
                set->complete = store_target_enqueue_indirect(set,
                    ir_operand_value(&definition->operands[i]));
            break;
        default:
            if (store_target_carries_reference(set, value->type)) {
                ir_annot_t *aliases = ir_annot_get(
                    ir_value_annots(value),
                    CORE_STRING_STATIC("ir.alias_count"));
                if (aliases)
                    set->complete = store_target_annotation_edges(set, value);
                else if (store_target_reference_is_writable(value)
                         || set->writable_projection)
                    set->external_writable = true;
                else
                    set->complete = false;
            }
            else
                set->complete = store_target_finish(set, value);
            break;
        }
    }
    return set->complete;
}

static void store_target_set_free(store_target_set_t *set)
{
    core_list_free(set->pending);
    core_list_free(set->targets);
    core_list_free(set->containers);
}

bool ir_value_visit_store_targets(const ir_value_t *address,
                                  ir_store_target_visitor_fn visitor,
                                  void *context)
{
    th_type *type = address && address->type
        ? th_resolve(address->type) : nullptr;
    if (!type || th_kind_of(type) != TH_INDIRECTION || !visitor)
        return false;
    store_target_set_t set;
    bool complete = store_target_collect(&set, (ir_value_t *)address, false,
                                         true);
    for (size_t i = 0; complete && i < set.targets.len; i++)
        complete = visitor(context, set.targets.data[i]);
    store_target_set_free(&set);
    return complete;
}

bool ir_value_visit_store_origins(const ir_value_t *address,
                                  ir_store_target_visitor_fn visitor,
                                  void *context)
{
    th_type *type = address && address->type
        ? th_resolve(address->type) : nullptr;
    if (!address || !visitor
        || (address->kind != IR_VAL_GLOBAL
            && (!type || th_kind_of(type) != TH_INDIRECTION)))
        return false;
    store_target_set_t set;
    bool complete = store_target_collect(&set, (ir_value_t *)address, true,
                                         true);
    for (size_t i = 0; complete && i < set.targets.len; i++)
        complete = visitor(context, set.targets.data[i]);
    store_target_set_free(&set);
    return complete;
}

static bool store_targets_include(const ir_value_t *address,
                                  const ir_value_t *target)
{
    store_target_set_t set;
    bool complete = store_target_collect(&set, (ir_value_t *)address, false,
                                         false);
    bool found = false;
    for (size_t i = 0; complete && i < set.targets.len; i++)
        found |= set.targets.data[i] == target;
    store_target_set_free(&set);
    return complete && found;
}

static bool initialization_binding_can_write(const ir_value_t *binding,
                                              ir_execution_phase_t phase)
{
    if (binding->kind == IR_VAL_GLOBAL && binding->global)
        return ir_global_can_write(binding->global, phase);
    return ir_value_can_write(binding, phase);
}

static bool initialization_binding_can_initialize(
    const ir_value_t *binding, ir_execution_phase_t phase)
{
    if (binding->kind == IR_VAL_GLOBAL && binding->global)
        return ir_global_can_initialize(binding->global, phase);
    return ir_value_can_initialize(binding, phase);
}

static bool initialization_event(const ir_instr_t *instruction,
                                 const ir_value_t *binding,
                                 ir_func_t *function,
                                 ir_execution_phase_t phase)
{
    if (!binding || !ir_instr_is_binding_initialization(instruction))
        return false;
    bool matches = instruction->op == IR_OP_COPY
        && instruction->dest_kind == IR_DEST_LOCAL
        && instruction->dest_local
        && ir_local_value(function, instruction->dest_local) == binding;
    if (instruction->op == IR_OP_STORE && instruction->n_operands)
        matches = store_targets_include(
            ir_operand_value(&instruction->operands[0]), binding);
    else if (instruction->op == IR_OP_GLOBAL_STORE && instruction->n_operands
             && binding->kind == IR_VAL_GLOBAL && binding->global) {
        ir_value_t *target = ir_operand_value(&instruction->operands[0]);
        ir_global_t *global = target && target->kind == IR_VAL_GLOBAL
            ? target->global ? target->global
                             : ir_global_find(function->owner,
                                              target->name)
            : nullptr;
        matches = global == binding->global;
    }
    return matches && !initialization_binding_can_write(binding, phase)
        && initialization_binding_can_initialize(binding, phase);
}

static ptrdiff_t init_event_index(ir_instr_t *instruction,
                                  ir_instr_t **events, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (events[i] == instruction) return (ptrdiff_t)i;
    return -1;
}

static bool binding_reentry_header(V *v, ir_func_t *function,
                                   ir_doms_t *graph, ir_block_t *entry,
                                   const ir_value_t *binding,
                                   ir_block_t **out)
{
    *out = nullptr;
    ir_annot_t *annot = ir_annot_get(
        ir_value_annots((ir_value_t *)binding),
        CORE_STRING_STATIC("ir.binding_reentry_header"));
    if (!annot) return true;
    ir_block_t *header = annot->kind == IR_ANNOT_STR && annot->s.data
        ? ir_block_find(function, annot->s) : nullptr;
    bool has_backedge = false;
    for (uint32_t i = 0; header && i < ir_pred_count(graph, header); i++) {
        ir_block_t *pred = ir_pred_at(graph, header, i);
        has_backedge |= pred && ir_block_reachable(graph, pred)
            && ir_dominates(graph, header, pred);
    }
    if (!header || !ir_block_reachable(graph, header) || !has_backedge) {
        broke(v, IR_DIAG_VERIFY_PHASE, nullptr, entry,
              "phase.binding-reentry-region",
              "binding %.*s names an invalid loop re-entry header",
              SARG(binding->name.data ? binding->name
                                      : CORE_STRING_STATIC("(unnamed)")));
        return false;
    }
    *out = header;
    return true;
}

static void verify_binding_initialization_events(
    V *v, ir_func_t *function, ir_doms_t *graph, ir_block_t *entry,
    ir_value_t *binding)
{
    uint32_t block_count = function->n_blocks;
    ir_execution_phase_t phase = ir_func_execution_phase(function);
    core_string_t name = ir_value_display_name(binding);
    if (initialization_binding_can_write(binding, phase)
        || !initialization_binding_can_initialize(binding, phase)) return;

    ir_block_t *reentry_header = nullptr;
    if (!binding_reentry_header(v, function, graph, entry, binding,
                                &reentry_header)) return;

    core_list_t(verify_instr_ptr_t) events = {0};
    for (uint32_t b = 0; b < block_count; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; ir_block_reachable(graph, block)
             && i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (!initialization_event(instruction, binding, function, phase))
                continue;
            if (!core_list_try_push(events, instruction)) {
                broke(v, IR_DIAG_VERIFY_PHASE, instruction, block,
                      "phase.binding-initialization-analysis",
                      "could not analyze binding initialization paths");
                goto done;
            }
        }
    }
    if (events.len < 2) goto done;

    size_t words = (events.len + 63) / 64;
    if (block_count > SIZE_MAX / words / sizeof(uint64_t)) {
        broke(v, IR_DIAG_VERIFY_PHASE, nullptr, entry,
              "phase.binding-initialization-analysis",
              "binding initialization analysis exceeds available capacity");
        goto done;
    }
    size_t cells = (size_t)block_count * words;
    uint64_t *in = calloc(cells, sizeof(*in));
    uint64_t *out = calloc(cells, sizeof(*out));
    uint64_t *state = calloc(words, sizeof(*state));
    if (!in || !out || !state) {
        free(in);
        free(out);
        free(state);
        broke(v, IR_DIAG_VERIFY_PHASE, nullptr, entry,
              "phase.binding-initialization-analysis",
              "could not analyze binding initialization paths");
        goto done;
    }

    bool changed;
    do {
        changed = false;
        for (uint32_t b = 0; b < block_count; b++) {
            ir_block_t *block = function->blocks[b];
            if (!ir_block_reachable(graph, block)) continue;
            uint64_t *block_in = in + (size_t)b * words;
            uint64_t *block_out = out + (size_t)b * words;
            for (uint32_t p = 0; p < ir_pred_count(graph, block); p++) {
                ir_block_t *pred = ir_pred_at(graph, block, p);
                if (!pred || !ir_block_reachable(graph, pred)) continue;
                if (block == reentry_header
                    && ir_dominates(graph, reentry_header, pred)) continue;
                uint64_t *pred_out = out + (size_t)pred->index * words;
                for (size_t w = 0; w < words; w++) {
                    uint64_t merged = block_in[w] | pred_out[w];
                    if (merged != block_in[w]) {
                        block_in[w] = merged;
                        changed = true;
                    }
                }
            }
            for (size_t w = 0; w < words; w++) {
                uint64_t merged = block_out[w] | block_in[w];
                if (merged != block_out[w]) {
                    block_out[w] = merged;
                    changed = true;
                }
            }
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                ptrdiff_t event = init_event_index(block->instrs[i],
                                                   events.data, events.len);
                if (event < 0) continue;
                size_t word = (size_t)event / 64;
                uint64_t bit = UINT64_C(1) << ((size_t)event % 64);
                if (!(block_out[word] & bit)) {
                    block_out[word] |= bit;
                    changed = true;
                }
            }
        }
    } while (changed);

    bool diagnosed = false;
    for (uint32_t b = 0; b < block_count && !diagnosed; b++) {
        ir_block_t *block = function->blocks[b];
        if (!ir_block_reachable(graph, block)) continue;
        memcpy(state, in + (size_t)b * words, words * sizeof(*state));
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            ptrdiff_t event = init_event_index(instruction, events.data,
                                               events.len);
            if (event < 0) continue;
            size_t event_word = (size_t)event / 64;
            uint64_t event_bit = UINT64_C(1) << ((size_t)event % 64);
            bool prior_distinct_site = false;
            for (size_t w = 0; w < words; w++) {
                uint64_t prior = state[w];
                if (w == event_word) prior &= ~event_bit;
                prior_distinct_site |= prior != 0;
            }
            if (prior_distinct_site) {
                broke(v, IR_DIAG_VERIFY_PHASE, instruction, block,
                      "phase.binding-initialized-once",
                      "immutable binding %.*s has multiple initialization sites on a reachable path",
                      SARG(name));
                diagnosed = true;
                break;
            }
            state[event_word] |= event_bit;
        }
    }
    free(in);
    free(out);
    free(state);
done:
    core_list_free(events);
}

static void verify_single_binding_initialization(V *v, ir_func_t *function)
{
    uint32_t block_count = function->n_blocks;
    ir_doms_t *graph = doms(v, function);
    ir_block_t *entry = ir_block_entry(function);
    if (!graph || !entry || entry->index >= block_count) {
        if (function->n_blocks)
            broke(v, IR_DIAG_VERIFY_PHASE, nullptr, entry,
                  "phase.binding-initialization-analysis",
                  "could not analyze binding initialization paths");
        return;
    }

    for (uint32_t i = 0; i < function->n_locals; i++) {
        ir_local_t *local = function->locals[i];
        verify_binding_initialization_events(v, function, graph, entry,
            ir_local_value(function, local));
    }
    for (uint32_t b = 0; b < block_count; b++) {
        ir_block_t *block = function->blocks[b];
        if (!ir_block_reachable(graph, block)) continue;
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (instruction->op == IR_OP_ALLOC && instruction->dest)
                verify_binding_initialization_events(v, function, graph,
                    entry, instruction->dest);
        }
    }
    for (size_t i = 0; i < ir_global_count(function->owner); i++) {
        ir_global_t *global = ir_global_at(function->owner, i);
        ir_value_t binding = {
            .kind = IR_VAL_GLOBAL,
            .name = global->name,
            .type = global->type,
            .global = global,
        };
        verify_binding_initialization_events(v, function, graph, entry,
                                             &binding);
    }
}

static bool typed_func(ir_visit_t *vi)
{
    verify_single_binding_initialization(vi->user, vi->func);
    return true;
}

static bool typed_instr(ir_visit_t *vi)
{
    V *v = vi->user;

    if (vi->instr->op == IR_OP_PHASE_RESULT
        && ir_func_execution_phase(vi->func) != IR_PHASE_RUNTIME
        && ir_func_execution_phase(vi->func) != IR_PHASE_COMPTIME) {
        broke(v, IR_DIAG_VERIFY_PHASE, vi->instr, vi->block,
              "phase.result-consumer-phase",
              "phase.result values require a compile-time or runtime consumer");
    }

    if (ir_module_op_info(v->m, vi->instr->op)) {
        return true;
    }

    broke(v, IR_DIAG_MISSING_PROFILE, vi->instr, vi->block, "typed.extension",
          "this extension opcode is not one profile %.*s supplies",
          SARG(v->m->profile ? v->m->profile->name
                             : CORE_STRING_STATIC("(none)")));

    return false;
}

static bool typed_operand(ir_visit_t *vi)
{
    V          *v   = vi->user;
    ir_value_t *val = vi->operand->kind == IR_OPD_VALUE ||
                      vi->operand->kind == IR_OPD_SPREAD ||
                      vi->operand->kind == IR_OPD_FRESH
                          ? vi->operand->value : nullptr;

    if (val && !val->type) {
        broke(v, IR_DIAG_VERIFY_TYPED, vi->instr, vi->block, "typed.value",
              "operand %u has no type", vi->index);
    }

    if (val && val->kind == IR_VAL_GLOBAL) {
        if (!(vi->instr->op == IR_OP_GLOBAL_STORE && vi->index == 0)) {
            ir_global_t *global = val->global
                ? val->global : ir_global_find(v->m, val->name);
            ir_execution_phase_t phase = ir_func_execution_phase(vi->func);
            if (global && !ir_global_can_read(global, phase)) {
                broke(v, IR_DIAG_VERIFY_PHASE, vi->instr, vi->block,
                      "phase.global-read",
                      "global %.*s is unavailable during %s execution",
                      SARG(global->name),
                      phase == IR_PHASE_COMPTIME
                          ? "compile-time" : "runtime");
            }
        }
    }
    else if (val && !(vi->instr->op == IR_OP_ADDRESS_OF
                      && vi->index == 0)
             && !((vi->instr->op == IR_OP_STORE
                   || vi->instr->op == IR_OP_SEQUENCE_SET)
                  && vi->index == 0
                  && val->kind == IR_VAL_INSTR && val->def
                  && (val->def->op == IR_OP_ADDRESS_OF
                      || val->def->op == IR_OP_ALLOC))) {
        ir_execution_phase_t phase = ir_func_execution_phase(vi->func);
        if (!ir_value_can_read(val, phase)) {
            broke(v, IR_DIAG_VERIFY_PHASE, vi->instr, vi->block,
                  "phase.binding-read",
                  "binding %.*s is unavailable during %s execution",
                  SARG(val->name.data ? val->name
                                      : CORE_STRING_STATIC("(unnamed)")),
                  phase == IR_PHASE_COMPTIME ? "compile-time" : "runtime");
        }
    }

    return true;
}

static bool check_binding_write(V *v, ir_instr_t *in, ir_block_t *block,
                                ir_value_t *binding)
{
    if (!binding) return true;
    ir_execution_phase_t phase = ir_func_execution_phase(in->block->owner);
    bool allowed = ir_value_can_write(binding, phase)
        || (ir_instr_is_binding_initialization(in)
            && ir_value_can_initialize(binding, phase));
    if (allowed) return true;
    broke_at(v, IR_DIAG_VERIFY_PHASE, in, block,
             ir_instr_is_binding_initialization(in)
                 ? CORE_STRING_STATIC("phase.binding-initialize")
                 : CORE_STRING_STATIC("phase.binding-write"),
          "binding %.*s cannot be %s during %s execution",
          SARG(binding->name.data ? binding->name
                                  : CORE_STRING_STATIC("(unnamed)")),
          ir_instr_is_binding_initialization(in)
              ? "initialized" : "written",
          phase == IR_PHASE_COMPTIME ? "compile-time" : "runtime");
    return false;
}

static ir_mutability_t contents_owner_mutability(const ir_value_t *owner,
                                                 ir_execution_phase_t phase)
{
    if (owner->kind == IR_VAL_GLOBAL && owner->global)
        return phase == IR_PHASE_COMPTIME
            ? ir_global_comptime_mutability(owner->global)
            : owner->global->mutability;
    return owner->local ? owner->local->mutability : IR_MUT_MUTABLE;
}

static void check_contents_write(V *v, ir_instr_t *in, ir_block_t *block,
                                 const store_target_set_t *targets)
{
    ir_execution_phase_t phase = ir_func_execution_phase(in->block->owner);
    for (size_t i = 0; i < targets->containers.len; i++) {
        store_target_set_t owners;
        bool complete = store_target_collect(&owners,
            targets->containers.data[i], false, true);
        for (size_t j = 0; complete && j < owners.targets.len; j++) {
            ir_value_t *owner = owners.targets.data[j];
            if (contents_owner_mutability(owner, phase) != IR_MUT_CONST)
                continue;
            broke(v, IR_DIAG_VERIFY_PHASE, in, block,
                  "phase.constant-contents-write",
                  "the contents of constant %.*s cannot be written during %s execution",
                  SARG(owner->global ? owner->global->name
                       : owner->name.data ? owner->name
                                          : CORE_STRING_STATIC("(unnamed)")),
                  phase == IR_PHASE_COMPTIME ? "compile-time" : "runtime");
        }
        store_target_set_free(&owners);
    }
}

static void check_store_target_write(V *v, ir_instr_t *in,
                                     ir_block_t *block,
                                     ir_value_t *address)
{
    store_target_set_t targets;
    bool complete = store_target_collect(&targets, address, false, false);
    bool pointer_address = address && address->type
        && th_kind_of(address->type) == TH_INDIRECTION;
    if (!complete || (pointer_address && !targets.targets.len
                      && !targets.external_writable && !targets.contents)) {
        broke(v, IR_DIAG_VERIFY_PHASE, in, block,
              "phase.store-alias-incomplete",
              "cannot verify a store through an incomplete alias target set");
        store_target_set_free(&targets);
        return;
    }
    check_contents_write(v, in, block, &targets);
    if (!targets.targets.len) {
        if (!targets.external_writable && !targets.contents)
            check_binding_write(v, in, block, address);
        store_target_set_free(&targets);
        return;
    }

    for (size_t i = 0; i < targets.targets.len; i++) {
        ir_value_t *target = targets.targets.data[i];
        if (target->kind == IR_VAL_GLOBAL) {
            ir_global_t *global = target->global
                ? target->global : ir_global_find(v->m, target->name);
            ir_func_t *function = in->block->owner;
            ir_execution_phase_t phase = ir_func_execution_phase(function);
            if (!global || (!ir_global_can_write(global, phase)
                && !((ir_instr_is_binding_initialization(in)
                      || ir_func_is_initializer(function))
                     && ir_global_can_initialize(global, phase)))) {
                broke(v, IR_DIAG_VERIFY_PHASE, in, block,
                      "phase.binding-write",
                      "global %.*s cannot be written through its address during %s execution",
                      SARG(global ? global->name
                           : target->name.data
                               ? target->name
                               : CORE_STRING_STATIC("(unnamed)")),
                      phase == IR_PHASE_COMPTIME
                          ? "compile-time" : "runtime");
            }
        }
        else {
            check_binding_write(v, in, block, target);
        }
    }
    store_target_set_free(&targets);
}

static bool typed_instr_end(ir_visit_t *vi)
{
    V          *v  = vi->user;
    ir_instr_t *in = vi->instr;

    if (!ir_module_op_info(v->m, in->op)) {
        return true;
    }

    typed_allocation_storage(vi);

    if (in->op == IR_OP_COPY && in->dest_kind == IR_DEST_LOCAL
        && in->dest_local)
        check_binding_write(v, in, vi->block,
                           ir_local_value(vi->func, in->dest_local));
    else if ((in->op == IR_OP_STORE || in->op == IR_OP_SEQUENCE_SET)
             && in->n_operands) {
        check_store_target_write(v, in, vi->block,
                                 ir_operand_value(&in->operands[0]));
    }

    ir_verdict_t verdict = ir_rule_apply(v->m, ir_module_rules(v->m), in,
                                         v->d).verdict;

    if (verdict == IR_RULE_DECLINE) {
        broke(v, IR_DIAG_VERIFY_TYPED, in, vi->block, "typed.rule",
              "no typing rule resolves for %.*s with these operand types",
              SARG(ir_module_op_spelling(v->m, in->op)));
    }
    else if (verdict != IR_RULE_ACCEPT) {
        v->bad = true;
    }

    return true;
}

static bool lifetime_instr(ir_visit_t *vi)
{
    ir_instr_t *in = vi->instr;
    if (in->op != IR_OP_FINALIZE && in->op != IR_OP_DEALLOC)
        return true;

    ir_value_t *value = in->n_operands
        ? ir_operand_value(&in->operands[0]) : nullptr;
    if (!value) return true;

    ir_liveness_t direct = ir_value_liveness_after(value, in);
    if (direct == IR_LIVENESS_LIVE) {
        broke(vi->user, IR_DIAG_VERIFY_LIFETIME, in, vi->block,
              "lifetime.no-live-use",
              "%.*s has a reachable use after this %s",
              SARG(value->name.data ? value->name
                                    : CORE_STRING_STATIC("the value")),
              in->op == IR_OP_FINALIZE ? "finalize" : "dealloc");
        return true;
    }
    if (direct == IR_LIVENESS_UNKNOWN) {
        broke(vi->user, IR_DIAG_VERIFY_LIFETIME, in, vi->block,
              "lifetime.unknown-direct",
              "cannot prove that %.*s has no reachable use after this %s",
              SARG(value->name.data ? value->name
                                    : CORE_STRING_STATIC("the value")),
              in->op == IR_OP_FINALIZE ? "finalize" : "dealloc");
        return true;
    }

    ir_liveness_t aliases = ir_value_alias_liveness_after(value, in);
    if (aliases == IR_LIVENESS_LIVE) {
        broke(vi->user, IR_DIAG_VERIFY_LIFETIME, in, vi->block,
              "lifetime.no-live-alias",
              "an alias of %.*s has a reachable use after this %s",
              SARG(value->name.data ? value->name
                                    : CORE_STRING_STATIC("the value")),
              in->op == IR_OP_FINALIZE ? "finalize" : "dealloc");
    }
    else if (aliases == IR_LIVENESS_UNKNOWN) {
        broke(vi->user, IR_DIAG_VERIFY_LIFETIME, in, vi->block,
              "lifetime.unknown-alias",
              "cannot prove that %.*s has no live aliases at this %s",
              SARG(value->name.data ? value->name
                                    : CORE_STRING_STATIC("the value")),
              in->op == IR_OP_FINALIZE ? "finalize" : "dealloc");
    }

    return true;
}

static bool ssa_func(ir_visit_t *vi)
{
    V *v = vi->user;

    for (uint32_t i = 0; i < vi->func->n_locals; i++) {
        ir_local_t *local = vi->func->locals[i];
        if (!local->address_taken) {
            broke(v, IR_DIAG_VERIFY_SSA, nullptr, ir_block_entry(vi->func),
                  "ssa.locals",
                  "%.*s is in SSA form but local $%.*s is not address-taken",
                  SARG(vi->func->name), SARG(local->name));
        }
    }

    return true;
}

static bool ssa_instr(ir_visit_t *vi)
{
    V *v = vi->user;

    if (vi->instr->dest_kind == IR_DEST_LOCAL
        && (!vi->instr->dest_local
            || !vi->instr->dest_local->address_taken)) {
        broke(v, IR_DIAG_VERIFY_SSA, vi->instr, vi->block, "ssa.locals",
              "this instruction writes local $%.*s in SSA form",
              SARG(vi->instr->dest_local ? vi->instr->dest_local->name
                                         : CORE_STRING_STATIC("?")));
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
                      : od->kind == IR_OPD_FRESH    ? od->value
                      : od->kind == IR_OPD_NAMED_ARG ? od->named.value
                      : od->kind == IR_OPD_PHI_ARM   ? od->phi.value
                                                      : nullptr;

    if (val && val->kind == IR_VAL_LOCAL
        && (!val->local || !val->local->address_taken)) {
        broke(v, IR_DIAG_VERIFY_SSA, in, b, "ssa.locals",
              "this instruction reads local $%.*s in SSA form",
              SARG(val->local ? val->local->name : CORE_STRING_STATIC("?")));
        return true;
    }

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
              "%%%.*s is used here but its definition in block %.*s does not "
              "dominate %.*s", SARG(val->name), SARG(val->def->block->name),
              SARG(from->name));

        return true;
    }

    if (val->def->block == from && od->kind != IR_OPD_PHI_ARM
        && val->def->index >= in->index) {
        broke(v, IR_DIAG_VERIFY_SSA, in, b, "ssa.dominance",
              "%%%.*s is used before it is defined in block %.*s",
              SARG(val->name), SARG(b->name));
    }

    return true;
}

static bool settled_for(V *v, th_type *t, uint64_t *bytes)
{
    ir_descriptor_t *d = ir_layout_of(v->m, t, (core_string_t){});

    if (!d || !ir_layout_is_settled(&d->size)) {
        return false;
    }
    *bytes = d->size.value;

    return true;
}

typedef struct {
    uint64_t      at;
    uint64_t      end;
    core_string_t name;
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

    ir_descriptor_t *d = ir_layout_of(v->m, t, (core_string_t){});
    core_string_t    target = v->m->target.data
        ? v->m->target : CORE_STRING_STATIC("(none)");

    if (!d) {
        broke(v, IR_DIAG_VERIFY_LAID_OUT, in, b, "laid-out.descriptor",
              "%.*s has no descriptor for target %.*s",
              SARG(ir_type_text(v->m, t)), SARG(target));

        return;
    }

    if (!ir_layout_is_settled(&d->size) || !ir_layout_is_settled(&d->align)) {
        broke(v, IR_DIAG_VERIFY_LAID_OUT, in, b, "laid-out.descriptor",
              "the %s of %.*s is still %.*s for target %.*s",
              ir_layout_is_settled(&d->size) ? "alignment" : "size",
              SARG(ir_type_text(v->m, t)),
              SARG(ir_layout_state_name(ir_layout_is_settled(&d->size)
                                            ? d->align.state
                                            : d->size.state)),
              SARG(target));
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
                  "members %.*s and %.*s of %.*s share bits, and no niche "
                  "encoding says they may", SARG(s[i].name),
                  SARG(s[i + 1].name), SARG(ir_type_text(v->m, t)));
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

    if ((od->kind == IR_OPD_VALUE || od->kind == IR_OPD_SPREAD
         || od->kind == IR_OPD_FRESH) && od->value) {
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
              "bitcast turns %llu bytes of %.*s into %llu bytes of %.*s",
              (unsigned long long)from,
              SARG(ir_type_text(v->m, in->operands[0].value->type)),
              (unsigned long long)to,
              SARG(ir_type_text(v->m, in->dest->type)));
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
                  "the edge from %.*s to %.*s is critical: %.*s has several "
                  "successors and %.*s several predecessors", SARG(b->name),
                  SARG(to->name), SARG(b->name), SARG(to->name));
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
              "structured.overlay", "%.*s has no region overlay",
              SARG(f->name));

        return false;
    }

    uint32_t *seen = ir_alloc(v->m->ctx, f->n_blocks * sizeof(uint32_t));

    tally(root, seen, f->n_blocks);

    for (uint32_t i = 0; i < f->n_blocks; i++) {
        if (seen[f->blocks[i]->index] != 1) {
            broke(v, IR_DIAG_VERIFY_STRUCTURE, nullptr, f->blocks[i],
                  "structured.overlay",
                  "the overlay covers block %.*s %u times, and must cover it "
                  "exactly once", SARG(f->blocks[i]->name),
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
                                       CORE_STRING_STATIC(
                                           "reported without a location"));

        if (bug) {
            ir_diag_message(bug, v->d,
                            CORE_STRING_STATIC(
                                "a diagnostic was reported with no primary "
                                "span, which is a bug in whatever reported "
                                "it"));
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
    ir_walk_on_global(w, typed_global, nullptr);
    ir_walk_on_func(w, typed_func, nullptr, nullptr);
    ir_walk_on_param(w, typed_param, nullptr);
    ir_walk_on_instr(w, typed_instr, typed_instr_end, nullptr);
    ir_walk_on_instr(w, lifetime_instr, nullptr, nullptr);
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
                                      ir_module_span(m),
                                      CORE_STRING_STATIC("claimed here"));

        if (dg) {
            core_buffer_t *sb = core_buffer_empty();

            core_buffer_printf(sb, "module %.*s claims %.*s, which the "
                               "verifier finds does not hold",
                               SARG(m->name), SARG(ir_prop_name(bit)));

            core_string_t msg = core_buffer_take_string(sb);

            ir_diag_message(dg, out, msg);
            core_free(msg.data);
        }
    }

    if ((held & IR_PROP_PARSED) && (held & IR_PROP_TYPED)
        && !((claimed & checked) & ~held)) {
        held |= IR_PROP_VERIFIED;
    }

    return held;
}

typedef struct {
    ir_span_t     span;
    ir_instr_t   *in;
    ir_block_t   *b;
    core_string_t what;
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
                     ir_instr_t *in, ir_block_t *b, core_string_t what)
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
             CORE_STRING_STATIC("a function"));

    return true;
}

static bool snap_local(ir_visit_t *vi)
{
    snap_one(vi->module->ctx, vi->user, vi->local->span, nullptr, nullptr,
             CORE_STRING_STATIC("a local"));

    return true;
}

static bool snap_block(ir_visit_t *vi)
{
    snap_one(vi->module->ctx, vi->user, vi->block->span, nullptr, vi->block,
             CORE_STRING_STATIC("a block"));

    return true;
}

static bool snap_instr(ir_visit_t *vi)
{
    snap_one(vi->module->ctx, vi->user, vi->instr->span, vi->instr, vi->block,
             CORE_STRING_STATIC("an instruction"));

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

bool ir_loc_monotone(ir_locsnap_t *before, ir_module_t *m,
                     core_string_t agent, ir_diags_t *out)
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
                                         r->b, nullptr,
                                         CORE_STRING_STATIC("here"));

        if (dg) {
            core_buffer_t *sb = core_buffer_empty();

            core_buffer_printf(sb, "%.*s had a known location before %.*s "
                               "ran and has none after", SARG(r->what),
                               SARG(agent.data ? agent
                                               : CORE_STRING_STATIC("a pass")));

            core_string_t msg = core_buffer_take_string(sb);

            ir_diag_message(dg, out, msg);
            core_free(msg.data);
        }
    }
    core_dict_free(&here);

    return ok;
}
