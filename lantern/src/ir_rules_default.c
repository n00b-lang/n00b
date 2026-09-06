#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/rule.h"
#include "ir/layout.h"
#include "ir/text.h"

#define ACCEPT(t)  ((ir_rule_result_t){.verdict = IR_RULE_ACCEPT, .result = (t)})
#define REJECT     ((ir_rule_result_t){.verdict = IR_RULE_REJECT})
#define DECLINE    ((ir_rule_result_t){.verdict = IR_RULE_DECLINE})

static th_type *opd_type(ir_instr_t *in, uint32_t i)
{
    if (!in || i >= in->n_operands) {
        return nullptr;
    }

    const ir_operand_t *o = &in->operands[i];

    switch (o->kind) {
    case IR_OPD_VALUE:
        return o->value ? o->value->type : nullptr;
    case IR_OPD_NAMED_ARG:
        return o->named.value ? o->named.value->type : nullptr;
    case IR_OPD_PHI_ARM:
        return o->phi.value ? o->phi.value->type : nullptr;
    case IR_OPD_TYPE:
        return o->type;
    default:
        return nullptr;
    }
}

static th_type *declared(ir_instr_t *in)
{
    if (in->dest_kind == IR_DEST_VALUE && in->dest) {
        return in->dest->type;
    }

    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local) {
        return in->dest_local->type;
    }

    return nullptr;
}

static const char *typed(ir_module_t *m, th_type *t)
{
    const char *s = t ? ir_type_text(m, t) : nullptr;

    return s ? s : "an unknown type";
}

__attribute__((format(printf, 6, 7))) static ir_diag_t *
complain(ir_module_t *m, ir_diags_t *d, ir_diagcode_t code, ir_instr_t *in,
         const char *label, const char *fmt, ...)
{
    (void)m;

    if (!d) {
        return nullptr;
    }

    ir_diag_t *dg = ir_diag_begin_at(d, code, in, in ? in->block : nullptr,
                                     in && in->block ? in->block->owner
                                                     : nullptr,
                                     label);

    if (!dg) {
        return nullptr;
    }

    ncc_buffer_t *b = ncc_buffer_empty();
    va_list ap;

    va_start(ap, fmt);
    ncc_buffer_vprintf(b, fmt, ap);
    va_end(ap);

    char *msg = ncc_buffer_take(b);

    ir_diag_message(dg, d, msg ? msg : fmt);
    ncc_free(msg);

    return dg;
}

static void blame(ir_diags_t *d, ir_diag_t *dg, ir_instr_t *in, uint32_t i,
                  const char *label)
{
    if (!dg || !in || i >= in->n_operands) {
        return;
    }

    const ir_operand_t *o = &in->operands[i];
    const ir_value_t   *v = o->kind == IR_OPD_VALUE      ? o->value
                          : o->kind == IR_OPD_NAMED_ARG  ? o->named.value
                          : o->kind == IR_OPD_PHI_ARM    ? o->phi.value
                                                         : nullptr;

    if (v && v->def && ir_span_known(v->def->span)) {
        ir_diag_secondary(d, dg, v->def->span, label);
    }
}

static bool same(ir_module_t *m, th_type *a, th_type *b)
{
    return a && b && ir_type_eq(m, a, b);
}

static bool is_int(ir_class_t c)
{
    return c == IR_CLASS_SINT || c == IR_CLASS_UINT;
}

static bool has_any_mod(ir_instr_t *in, const char *const *names, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (ir_instr_has_mod(in, names[i])) {
            return true;
        }
    }

    return false;
}

static const char *const FIT[]   = {"wrap", "trap", "sat", "undef"};
static const char *const ZERO[]  = {"trap", "undef"};
static const char *const ORDER[] = {"ordered", "unordered"};
static const char *const ROUND[] = {"to_zero", "nearest", "floor", "ceil"};

typedef struct {
    const char *const *names;
    size_t             n;
    const char        *what;
} axis_t;

static const axis_t AX_FIT   = {FIT, 4, "overflow"};
static const axis_t AX_ZERO  = {ZERO, 2, "divide-by-zero"};
static const axis_t AX_ROUND = {ROUND, 4, "rounding"};

static bool axis_has(const axis_t *ax, const char *name)
{
    for (size_t i = 0; i < ax->n; i++) {
        if (!strcmp(ax->names[i], name)) {
            return true;
        }
    }

    return false;
}

static bool assign_axes(ir_instr_t *in, const axis_t *const *ax, size_t n_ax,
                        size_t k, uint32_t used)
{
    if (k == n_ax) {
        return true;
    }

    for (uint32_t i = 0; i < in->n_mods; i++) {
        if ((used & (1u << i)) || !axis_has(ax[k], in->mods[i])) {
            continue;
        }

        if (assign_axes(in, ax, n_ax, k + 1, used | (1u << i))) {
            return true;
        }
    }

    return false;
}

static void say_axes(ir_module_t *m, ir_diags_t *d, ir_instr_t *in,
                     const axis_t *const *ax, size_t n_ax)
{
    ncc_buffer_t *b = ncc_buffer_empty();

    if (!n_ax) {
        ncc_buffer_printf(b, "%s takes no modifiers here",
                  ir_module_op_spelling(m, in->op));
    }
    else {
        ncc_buffer_printf(b, "%s takes exactly one ", ir_module_op_spelling(m, in->op));

        for (size_t i = 0; i < n_ax; i++) {
            if (i) {
                ncc_buffer_puts(b, i + 1 == n_ax ? " and one " : ", one ");
            }
            ncc_buffer_puts(b, ax[i]->what);
        }
        ncc_buffer_puts(b, n_ax == 1 ? " modifier" : " modifier of each");
    }

    char *msg = ncc_buffer_take(b);

    complain(m, d, IR_DIAG_TYPE_MODIFIER, in, "these modifiers", "%s",
             msg ? msg : "wrong modifiers");
    ncc_free(msg);
}

static bool mods_exact(ir_module_t *m, ir_instr_t *in, ir_diags_t *d,
                       const axis_t *const *ax, size_t n_ax)
{
    if (in->n_mods != n_ax || !assign_axes(in, ax, n_ax, 0, 0)) {
        say_axes(m, d, in, ax, n_ax);

        return false;
    }

    return true;
}

static ir_rule_result_t accept_const(ir_module_t *m, th_type *result,
                                     ir_annot_t a)
{
    ir_annot_t *slot = ir_alloc(m->ctx, sizeof(ir_annot_t));

    a.key          = "ir.const";
    a.derived_from = IR_PROP_TYPED;
    *slot          = a;

    return (ir_rule_result_t){
        .verdict  = IR_RULE_ACCEPT,
        .result   = result,
        .annots   = slot,
        .n_annots = 1,
    };
}

static ir_rule_result_t rule_arith(ir_module_t *m, ir_instr_t *in,
                                   ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);

    if (!a) {
        return DECLINE;
    }

    ir_class_t c = ir_machine_of(m, a).class;

    if (!is_int(c) && c != IR_CLASS_FLOAT) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "arithmetic needs an integer or floating-point operand, "
                 "but this one is %s", typed(m, a));

        return REJECT;
    }

    if (in->n_operands == 2 && !same(m, a, opd_type(in, 1))) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "these operands",
                           "arithmetic operands must have the same type, "
                           "but the first is %s and the second is %s",
                           typed(m, a), typed(m, opd_type(in, 1))),
              in, 1, "the second operand is defined here");

        return REJECT;
    }

    bool divides = in->op == IR_OP_DIV || in->op == IR_OP_REM;

    if (is_int(c)) {
        const axis_t *ax[2];
        size_t        n_ax = 0;

        if (divides) {
            ax[n_ax++] = &AX_ZERO;
        }

        if (!divides || c == IR_CLASS_SINT) {
            ax[n_ax++] = &AX_FIT;
        }

        if (!mods_exact(m, in, d, ax, n_ax)) {
            return REJECT;
        }
    }
    else if (in->n_mods) {
        complain(m, d, IR_DIAG_TYPE_MODIFIER, in, "this modifier",
                 "floating-point arithmetic takes no modifiers, but %s is "
                 "present", in->mods[0]);

        return REJECT;
    }

    return ACCEPT(a);
}

static ir_rule_result_t rule_bitwise(ir_module_t *m, ir_instr_t *in,
                                     ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);

    if (!a) {
        return DECLINE;
    }

    if (!is_int(ir_machine_of(m, a).class)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "bitwise operations need an integer operand, but this one "
                 "is %s", typed(m, a));

        return REJECT;
    }

    if (in->n_operands == 2 && !same(m, a, opd_type(in, 1))) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "these operands",
                           "bitwise operands must have the same type, but "
                           "the first is %s and the second is %s",
                           typed(m, a), typed(m, opd_type(in, 1))),
              in, 1, "the second operand is defined here");

        return REJECT;
    }

    return ACCEPT(a);
}

static ir_rule_result_t rule_shift(ir_module_t *m, ir_instr_t *in,
                                   ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);
    th_type *b = opd_type(in, 1);

    if (!a || !b) {
        return DECLINE;
    }

    if (!is_int(ir_machine_of(m, a).class)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "the value shifted",
                 "a shift needs an integer value, but this one is %s",
                 typed(m, a));

        return REJECT;
    }

    if (!is_int(ir_machine_of(m, b).class)) {
        blame(d, complain(m, d, IR_DIAG_TYPE_CLASS, in, "the shift amount",
                           "a shift amount must be an integer, but this one "
                           "is %s", typed(m, b)),
              in, 1, "the shift amount is defined here");

        return REJECT;
    }

    return ACCEPT(a);
}

static ir_rule_result_t rule_count(ir_module_t *m, ir_instr_t *in,
                                   ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);

    if (!a) {
        return DECLINE;
    }

    if (!is_int(ir_machine_of(m, a).class)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "a bit count needs an integer operand, but this one is %s",
                 typed(m, a));

        return REJECT;
    }

    th_type *u32 = ir_profile_scalar(m, IR_CLASS_UINT, 32);

    if (!u32) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "here",
                 "a bit count yields a 32-bit unsigned integer, and profile "
                 "%s declares no such type", m->profile->name);

        return REJECT;
    }

    return ACCEPT(u32);
}

static ir_rule_result_t rule_compare(ir_module_t *m, ir_instr_t *in,
                                     ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);

    if (!a) {
        return DECLINE;
    }

    if (!same(m, a, opd_type(in, 1))) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "these operands",
                           "comparison operands must have the same type, but "
                           "the first is %s and the second is %s",
                           typed(m, a), typed(m, opd_type(in, 1))),
              in, 1, "the second operand is defined here");

        return REJECT;
    }

    if (ir_machine_of(m, a).class == IR_CLASS_FLOAT) {
        if (!has_any_mod(in, ORDER, 2)) {
            complain(m, d, IR_DIAG_TYPE_MODIFIER, in, "this comparison",
                     "comparing %s needs the ordered or unordered modifier",
                     typed(m, a));

            return REJECT;
        }
    }
    else if (in->n_mods) {
        complain(m, d, IR_DIAG_TYPE_MODIFIER, in, "this modifier",
                 "%s is a float comparison modifier, and these operands are "
                 "%s", in->mods[0], typed(m, a));

        return REJECT;
    }

    th_type *b = ir_profile_scalar(m, IR_CLASS_BOOL, 0);

    if (!b) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "here",
                 "a comparison yields a boolean, and profile %s declares no "
                 "such type", m->profile->name);

        return REJECT;
    }

    return ACCEPT(b);
}

static th_type *record_of(ir_module_t *m, th_type *t)
{
    if (!t) {
        return nullptr;
    }

    if (th_kind_of(t) == TH_RECORD) {
        return t;
    }

    if (th_kind_of(t) == TH_NAMED) {
        th_type *def = ir_type_definition(m, t);

        if (def && th_kind_of(def) == TH_RECORD) {
            return def;
        }
    }

    return nullptr;
}

static bool int_fits(ir_lit_t v, ir_machine_t k)
{
    if (k.class == IR_CLASS_BOOL) {
        return !v.neg && v.mag <= 1;
    }

    if (k.width == 0 || k.width > 128) {
        return false;
    }

    if (k.class == IR_CLASS_UINT) {
        if (v.neg && v.mag) {
            return false;
        }

        return k.width == 128 || v.mag < ((ir_uint_t)1 << k.width);
    }

    ir_uint_t lim = (ir_uint_t)1 << (k.width - 1);

    return v.neg ? v.mag <= lim : v.mag < lim;
}

typedef enum : uint8_t {
    FLOAT_FITS,
    FLOAT_TOO_WIDE,
    FLOAT_UNCHECKABLE,
} float_verdict_t;

static float_verdict_t float_fits(double v, ir_machine_t k)
{
    if (k.width == 32) {
        return !isfinite(v) || isfinite((float)v) ? FLOAT_FITS
                                                 : FLOAT_TOO_WIDE;
    }

    return k.width >= 64 ? FLOAT_FITS : FLOAT_UNCHECKABLE;
}

static ir_rule_result_t rule_const(ir_module_t *m, ir_instr_t *in,
                                   ir_diags_t *d, th_type *t)
{
    ir_machine_t k = ir_machine_of(m, t);

    if (k.class == IR_CLASS_OPAQUE) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this constant",
                 "%s has no machine class in profile %s, so it has no "
                 "constants", typed(m, t), m->profile->name);

        return REJECT;
    }

    if (!in->n_operands || in->operands[0].kind != IR_OPD_CONST) {
        return ACCEPT(t);
    }

    bool is_float = in->operands[0].lit.is_float;

    if (is_float != (k.class == IR_CLASS_FLOAT)) {
        complain(m, d, IR_DIAG_TYPE_LITERAL, in, "this literal",
                 "%s literal in %s, which is %s",
                 is_float ? "a floating-point" : "an integer", typed(m, t),
                 is_float ? "not a floating-point type"
                          : "a floating-point type");

        return REJECT;
    }

    if (!is_float && !int_fits(in->operands[0].lit.i, k)) {
        char *lit = ir_lit_text(in->operands[0].lit.i);

        complain(m, d, IR_DIAG_TYPE_LITERAL, in, "this literal",
                 "%s does not fit in %s", lit, typed(m, t));
        ncc_free(lit);

        return REJECT;
    }

    if (is_float) {
        switch (float_fits(in->operands[0].lit.f, k)) {
        case FLOAT_TOO_WIDE:
            complain(m, d, IR_DIAG_TYPE_LITERAL, in, "this literal",
                     "%s is not representable in %s",
                     in->operands[0].lit.text ? in->operands[0].lit.text
                                              : "this value",
                     typed(m, t));

            return REJECT;

        case FLOAT_UNCHECKABLE:
            complain(m, d, IR_DIAG_TYPE_LITERAL, in, "this literal",
                     "%s is %u bits wide, and a literal can only be checked "
                     "against a 32-bit or wider floating-point type",
                     typed(m, t), k.width);

            return REJECT;

        default:
            break;
        }
    }

    return ACCEPT(t);
}

static ir_rule_result_t rule_aggregate(ir_module_t *m, ir_instr_t *in,
                                       ir_diags_t *d, th_type *t)
{
    th_type *rc = record_of(m, t);

    if (!rc) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this destination",
                 "aggregate builds a record, and %s is not one", typed(m, t));

        return REJECT;
    }

    size_t n = th_record_len(rc);

    if (in->n_operands != n) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "these operands",
                 "%s has %zu members and %u values were given", typed(m, t), n,
                 in->n_operands);

        return REJECT;
    }

    for (size_t i = 0; i < n; i++) {
        const th_member *mem = th_record_member(rc, i);

        if (!mem || !same(m, ir_type_intern(m, mem->type),
                          opd_type(in, (uint32_t)i))) {
            blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in,
                               "this member",
                               "member %s of %s is %s, and the value given "
                               "is %s", mem ? mem->name : "?", typed(m, t),
                               typed(m, mem ? mem->type : nullptr),
                               typed(m, opd_type(in, (uint32_t)i))),
                  in, (uint32_t)i, "the value is defined here");

            return REJECT;
        }
    }

    return ACCEPT(t);
}

static ir_rule_result_t rule_annotated(ir_module_t *m, ir_instr_t *in,
                                       ir_diags_t *d, void *state)
{
    (void)state;

    th_type *t = declared(in);

    if (!t) {
        complain(m, d, IR_DIAG_TYPE_DEST, in, "this instruction",
                 "%s takes its type from its destination, and has none",
                 ir_module_op_spelling(m, in->op));

        return REJECT;
    }

    if (in->op == IR_OP_CONST) {
        return rule_const(m, in, d, t);
    }

    if (in->op == IR_OP_AGGREGATE) {
        return rule_aggregate(m, in, d, t);
    }

    return ACCEPT(t);
}

static ir_rule_result_t rule_copy(ir_module_t *m, ir_instr_t *in,
                                  ir_diags_t *d, void *state)
{
    (void)m;
    (void)d;
    (void)state;

    th_type *a = opd_type(in, 0);

    return a ? ACCEPT(a) : DECLINE;
}

static ir_rule_result_t rule_alloca(ir_module_t *m, ir_instr_t *in,
                                    ir_diags_t *d, void *state)
{
    (void)state;

    th_type *t = opd_type(in, 0);

    if (!t) {
        return DECLINE;
    }

    if (in->n_operands > 1 && !is_int(ir_machine_of(m, opd_type(in, 1)).class)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this count",
                 "an alloca count must be an integer, and this one is %s",
                 typed(m, opd_type(in, 1)));

        return REJECT;
    }

    th_type *p = th_ind(ir_ctx_types(m->ctx),
                        TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE, t);

    if (!p) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this type",
                 "could not form a pointer to %s", typed(m, t));

        return REJECT;
    }

    return ACCEPT(ir_type_intern(m, p));
}

static ir_rule_result_t rule_load(ir_module_t *m, ir_instr_t *in,
                                  ir_diags_t *d, void *state)
{
    (void)state;

    th_type *p = opd_type(in, 0);

    if (!p) {
        return DECLINE;
    }

    th_type *to = th_ind_referent(p);

    if (!to) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this operand",
                 "load needs a pointer, and this one is %s", typed(m, p));

        return REJECT;
    }

    return ACCEPT(ir_type_intern(m, to));
}

static ir_rule_result_t rule_store(ir_module_t *m, ir_instr_t *in,
                                   ir_diags_t *d, void *state)
{
    (void)state;

    th_type *p = opd_type(in, 0);
    th_type *v = opd_type(in, 1);

    if (!p || !v) {
        return DECLINE;
    }

    th_type *to = th_ind_referent(p);

    if (!to) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this operand",
                 "store needs a pointer, and this one is %s", typed(m, p));

        return REJECT;
    }

    if (!same(m, ir_type_intern(m, to), v)) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this value",
                           "storing %s through a pointer to %s", typed(m, v),
                           typed(m, ir_type_intern(m, to))),
              in, 1, "the value is defined here");

        return REJECT;
    }

    return ACCEPT(nullptr);
}

static ir_rule_result_t rule_field(ir_module_t *m, ir_instr_t *in,
                                   ir_diags_t *d, void *state)
{
    (void)state;

    th_type *p = opd_type(in, 0);

    if (!p || in->n_operands < 2) {
        return DECLINE;
    }

    th_type *to = th_ind_referent(p);
    th_type *rc = record_of(m, to);

    if (!rc) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "field needs a pointer to a record, and this one is %s",
                 typed(m, p));

        return REJECT;
    }

    th_type *mt = th_record_lookup(rc, in->operands[1].name);

    if (!mt) {
        complain(m, d, IR_DIAG_TYPE_MEMBER, in, "this name",
                 "%s has no member %s", typed(m, rc), in->operands[1].name);

        return REJECT;
    }

    th_type *out = th_ind(ir_ctx_types(m->ctx),
                          TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE, mt);

    return ACCEPT(ir_type_intern(m, out));
}

static ir_rule_result_t rule_extract(ir_module_t *m, ir_instr_t *in,
                                     ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);

    if (!a || in->n_operands < 2) {
        return DECLINE;
    }

    th_type *rc = record_of(m, a);

    if (!rc) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "extract needs a record value, and this one is %s",
                 typed(m, a));

        return REJECT;
    }

    th_type *mt = th_record_lookup(rc, in->operands[1].name);

    if (!mt) {
        complain(m, d, IR_DIAG_TYPE_MEMBER, in, "this name",
                 "%s has no member %s", typed(m, rc), in->operands[1].name);

        return REJECT;
    }

    return ACCEPT(ir_type_intern(m, mt));
}

static ir_rule_result_t rule_insert(ir_module_t *m, ir_instr_t *in,
                                    ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);

    if (!a || in->n_operands < 3) {
        return DECLINE;
    }

    th_type *rc = record_of(m, a);

    if (!rc) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "insert needs a record value, and this one is %s",
                 typed(m, a));

        return REJECT;
    }

    th_type *mt = th_record_lookup(rc, in->operands[1].name);

    if (!mt) {
        complain(m, d, IR_DIAG_TYPE_MEMBER, in, "this name",
                 "%s has no member %s", typed(m, rc), in->operands[1].name);

        return REJECT;
    }

    if (!same(m, ir_type_intern(m, mt), opd_type(in, 2))) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this value",
                          "member %s of %s is %s, and the value given is %s",
                          in->operands[1].name, typed(m, a),
                          typed(m, ir_type_intern(m, mt)),
                          typed(m, opd_type(in, 2))),
              in, 2, "the value is defined here");

        return REJECT;
    }

    return ACCEPT(a);
}

static ir_rule_result_t rule_index(ir_module_t *m, ir_instr_t *in,
                                   ir_diags_t *d, void *state)
{
    (void)state;

    th_type *p = opd_type(in, 0);
    th_type *i = opd_type(in, 1);

    if (!p || !i) {
        return DECLINE;
    }

    if (!is_int(ir_machine_of(m, i).class)) {
        blame(d, complain(m, d, IR_DIAG_TYPE_CLASS, in, "this index",
                           "an index must be an integer, and this one is %s",
                           typed(m, i)),
              in, 1, "the index is defined here");

        return REJECT;
    }

    th_type *to    = th_ind_referent(p);
    uint8_t  attrs = 0;

    if (!to || !th_ind_attrs(p, &attrs)) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this operand",
                 "index needs a pointer, and this one is %s", typed(m, p));

        return REJECT;
    }

    th_type *elem = nullptr;

    if (th_kind_of(to) == TH_SEQUENCE) {
        elem = th_seq_elem(to);
    }
    else if (attrs & TH_INDEXABLE) {
        elem = to;
    }
    else {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this operand",
                 "%s is neither a sequence nor an indexable indirection",
                 typed(m, p));

        return REJECT;
    }

    th_type *out = th_ind(ir_ctx_types(m->ctx),
                           TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE, elem);

    return ACCEPT(ir_type_intern(m, out));
}

static ir_rule_result_t rule_offset(ir_module_t *m, ir_instr_t *in,
                                    ir_diags_t *d, void *state)
{
    (void)state;

    th_type *p = opd_type(in, 0);
    th_type *i = opd_type(in, 1);

    if (!p || !i) {
        return DECLINE;
    }

    uint8_t attrs = 0;

    if (!th_ind_attrs(p, &attrs)) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this operand",
                 "offset needs a pointer, and this one is %s", typed(m, p));

        return REJECT;
    }

    if (!(attrs & TH_INDEXABLE)) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this operand",
                 "offset needs an indexable indirection, and %s is not one",
                 typed(m, p));

        return REJECT;
    }

    if (!is_int(ir_machine_of(m, i).class)) {
        blame(d, complain(m, d, IR_DIAG_TYPE_CLASS, in, "this offset",
                           "an offset must be an integer, and this one is %s",
                           typed(m, i)),
              in, 1, "the offset is defined here");

        return REJECT;
    }

    return ACCEPT(p);
}

static ir_rule_result_t rule_diff(ir_module_t *m, ir_instr_t *in,
                                  ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);

    if (!a) {
        return DECLINE;
    }

    if (!th_ind_referent(a) || !same(m, a, opd_type(in, 1))) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "these operands",
                           "diff needs two pointers of one type, and these "
                           "are %s and %s", typed(m, a),
                           typed(m, opd_type(in, 1))),
              in, 1, "the second operand is defined here");

        return REJECT;
    }

    th_type *s = ir_profile_scalar(m, IR_CLASS_SINT, 64);

    if (!s) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "here",
                 "diff yields a 64-bit signed integer, and profile %s "
                 "declares no such type", m->profile->name);

        return REJECT;
    }

    return ACCEPT(s);
}

static ir_rule_result_t rule_mem(ir_module_t *m, ir_instr_t *in,
                                 ir_diags_t *d, void *state)
{
    (void)state;

    th_type *d0 = opd_type(in, 0);
    th_type *d1 = opd_type(in, 1);
    th_type *d2 = opd_type(in, 2);

    if (!d0 || !d1 || !d2) {
        return DECLINE;
    }

    if (ir_machine_of(m, d0).class != IR_CLASS_PTR) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this operand",
                 "%s writes through a pointer, and this one is %s",
                 ir_module_op_spelling(m, in->op), typed(m, d0));

        return REJECT;
    }

    if (in->op == IR_OP_MEMSET) {
        ir_machine_t v = ir_machine_of(m, d1);

        if (v.class != IR_CLASS_UINT || v.width != 8) {
            blame(d, complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                           "memset fills with an 8-bit unsigned value, and "
                           "this one is %s", typed(m, d1)),
              in, 1, "the fill value is defined here");

            return REJECT;
        }
    }
    else if (ir_machine_of(m, d1).class != IR_CLASS_PTR) {
        blame(d, complain(m, d, IR_DIAG_TYPE_POINTER, in, "this operand",
                           "%s reads through a pointer, and this one is %s",
                           ir_module_op_spelling(m, in->op), typed(m, d1)),
              in, 1, "the source is defined here");

        return REJECT;
    }

    if (!is_int(ir_machine_of(m, d2).class)) {
        blame(d, complain(m, d, IR_DIAG_TYPE_CLASS, in, "this length",
                           "%s takes an integer length, and this one is %s",
                           ir_module_op_spelling(m, in->op), typed(m, d2)),
              in, 2, "the length is defined here");

        return REJECT;
    }

    if (in->op != IR_OP_MEMCMP) {
        return ACCEPT(nullptr);
    }

    th_type *s = ir_profile_scalar(m, IR_CLASS_SINT, 32);

    if (!s) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "here",
                 "memcmp yields a 32-bit signed integer, and profile %s "
                 "declares no such type", m->profile->name);

        return REJECT;
    }

    return ACCEPT(s);
}

static ir_rule_result_t rule_variant_make(ir_module_t *m, ir_instr_t *in,
                                          ir_diags_t *d, void *state)
{
    (void)state;

    th_type *t = declared(in);
    bool     is_variant = false;

    if (!t) {
        complain(m, d, IR_DIAG_TYPE_DEST, in, "this instruction",
                 "variant.make takes its type from its destination, and has "
                 "none");

        return REJECT;
    }

    if (!th_named_info(t, nullptr, nullptr, &is_variant, nullptr)
        || !is_variant) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this destination",
                 "variant.make needs a variant type, and %s is not one",
                 typed(m, t));

        return REJECT;
    }

    if (!in->n_operands || in->operands[0].kind != IR_OPD_NAME) {
        return DECLINE;
    }

    const char *arm  = in->operands[0].name;
    th_type    *arms = record_of(m, t);

    if (arms) {
        th_type *payload = th_record_lookup(arms, arm);

        if (!payload) {
            complain(m, d, IR_DIAG_TYPE_ARM, in, "this name",
                     "%s has no arm %s", typed(m, t), arm);

            return REJECT;
        }

        if (!same(m, ir_type_intern(m, payload), opd_type(in, 1))) {
            blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this payload",
                              "arm %s of %s carries %s, and this value is %s",
                              arm, typed(m, t),
                              typed(m, ir_type_intern(m, payload)),
                              typed(m, opd_type(in, 1))),
                  in, 1, "the payload is defined here");

            return REJECT;
        }
    }

    return accept_const(m, t,
                        (ir_annot_t){.kind = IR_ANNOT_INT,
                                     .i    = ir_lit_hex(th_arm_tag(arm))});
}

static ir_rule_result_t rule_variant_tag(ir_module_t *m, ir_instr_t *in,
                                         ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);
    bool     is_variant = false;

    if (!a) {
        return DECLINE;
    }

    if (!th_named_info(a, nullptr, nullptr, &is_variant, nullptr)
        || !is_variant) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "variant.tag needs a variant value, and this one is %s",
                 typed(m, a));

        return REJECT;
    }

    th_type *u = ir_profile_scalar(m, IR_CLASS_UINT, 64);

    if (!u) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "here",
                 "variant.tag yields a 64-bit unsigned integer, and profile "
                 "%s declares no such type", m->profile->name);

        return REJECT;
    }

    return ACCEPT(u);
}

static ir_rule_result_t rule_variant_payload(ir_module_t *m, ir_instr_t *in,
                                             ir_diags_t *d, void *state)
{
    (void)state;

    th_type *a = opd_type(in, 0);
    bool     is_variant = false;

    if (!a || in->n_operands < 2) {
        return DECLINE;
    }

    if (!th_named_info(a, nullptr, nullptr, &is_variant, nullptr)
        || !is_variant) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "variant.payload needs a variant value, and this one is %s",
                 typed(m, a));

        return REJECT;
    }

    th_type *t = declared(in);

    if (!t) {
        complain(m, d, IR_DIAG_TYPE_DEST, in, "this instruction",
                 "variant.payload takes its arm type from its destination, "
                 "and has none");

        return REJECT;
    }

    const char *arm  = in->operands[1].name;
    th_type    *arms = record_of(m, a);

    if (arms) {
        th_type *payload = th_record_lookup(arms, arm);

        if (!payload) {
            complain(m, d, IR_DIAG_TYPE_ARM, in, "this name",
                     "%s has no arm %s", typed(m, a), arm);

            return REJECT;
        }

        if (!same(m, ir_type_intern(m, payload), t)) {
            complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this destination",
                     "arm %s of %s carries %s, and the destination is %s",
                     arm, typed(m, a), typed(m, ir_type_intern(m, payload)),
                     typed(m, t));

            return REJECT;
        }
    }

    return ACCEPT(t);
}

static ir_rule_result_t rule_cast(ir_module_t *m, ir_instr_t *in,
                                  ir_diags_t *d, void *state)
{
    (void)state;

    th_type *from = opd_type(in, 0);
    th_type *to   = declared(in);

    if (!from || !to) {
        complain(m, d, IR_DIAG_TYPE_DEST, in, "this instruction",
                 "a cast takes its target type from its destination, and has "
                 "none");

        return REJECT;
    }

    ir_machine_t f = ir_machine_of(m, from);
    ir_machine_t t = ir_machine_of(m, to);

    if (in->op == IR_OP_BITCAST) {
        return ACCEPT(to);
    }

    bool i2i = is_int(f.class) && is_int(t.class);
    bool f2i = f.class == IR_CLASS_FLOAT && is_int(t.class);
    bool i2f = is_int(f.class) && t.class == IR_CLASS_FLOAT;
    bool f2f = f.class == IR_CLASS_FLOAT && t.class == IR_CLASS_FLOAT;
    bool p2i = (f.class == IR_CLASS_PTR && is_int(t.class))
            || (is_int(f.class) && t.class == IR_CLASS_PTR);
    bool b2i = (f.class == IR_CLASS_BOOL && is_int(t.class))
            || (is_int(f.class) && t.class == IR_CLASS_BOOL);

    if (!i2i && !f2i && !i2f && !f2f && !p2i && !b2i) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this cast",
                 "cast does not convert %s to %s; bitcast reinterprets",
                 typed(m, from), typed(m, to));

        return REJECT;
    }

    const axis_t *ax[2];
    size_t        n_ax = 0;

    if (f2i || (i2i && t.width < f.width)) {
        ax[n_ax++] = &AX_FIT;
    }

    if (f2i || i2f || (f2f && t.width < f.width)) {
        ax[n_ax++] = &AX_ROUND;
    }

    if (!mods_exact(m, in, d, ax, n_ax)) {
        return REJECT;
    }

    return ACCEPT(to);
}

static ir_rule_result_t rule_select(ir_module_t *m, ir_instr_t *in,
                                    ir_diags_t *d, void *state)
{
    (void)state;

    th_type *c = opd_type(in, 0);
    th_type *a = opd_type(in, 1);

    if (!c || !a) {
        return DECLINE;
    }

    if (ir_machine_of(m, c).class != IR_CLASS_BOOL) {
        blame(d, complain(m, d, IR_DIAG_TYPE_CLASS, in, "this condition",
                           "select needs a boolean condition, and this one "
                           "is %s", typed(m, c)),
              in, 0, "the condition is defined here");

        return REJECT;
    }

    if (!same(m, a, opd_type(in, 2))) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "these arms",
                           "select arms must have one type, and these are %s "
                           "and %s", typed(m, a), typed(m, opd_type(in, 2))),
              in, 2, "the second arm is defined here");

        return REJECT;
    }

    return ACCEPT(a);
}

static ir_rule_result_t rule_phi(ir_module_t *m, ir_instr_t *in,
                                 ir_diags_t *d, void *state)
{
    (void)state;

    th_type *t = declared(in);

    if (!t) {
        complain(m, d, IR_DIAG_TYPE_DEST, in, "this phi",
                 "phi takes its type from its destination, and has none");

        return REJECT;
    }

    for (uint32_t i = 0; i < in->n_operands; i++) {
        th_type *a = opd_type(in, i);

        if (a && !same(m, a, t)) {
            blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this arm",
                           "this phi is %s and an incoming value is %s",
                           typed(m, t), typed(m, opd_type(in, i))),
              in, i, "the incoming value is defined here");

            return REJECT;
        }
    }

    return ACCEPT(t);
}

static bool arg_matches(ir_module_t *m, th_mode mode, th_type *want,
                        th_type *got)
{
    if (!want || !got) {
        return false;
    }

    if (mode == TH_IN) {
        return same(m, ir_type_intern(m, want), got);
    }

    th_type *to = th_ind_referent(got);

    return to && same(m, ir_type_intern(m, want), ir_type_intern(m, to));
}

static ir_rule_result_t rule_call(ir_module_t *m, ir_instr_t *in,
                                  ir_diags_t *d, void *state)
{
    (void)state;

    th_type *callee = opd_type(in, 0);

    if (!callee) {
        return DECLINE;
    }

    if (th_kind_of(callee) == TH_INDIRECTION) {
        callee = th_ind_referent(callee);
    }

    if (!callee || th_kind_of(callee) != TH_FUNCTION) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this callee",
                 "call needs a function or a pointer to one, and this is %s",
                 typed(m, opd_type(in, 0)));

        return REJECT;
    }

    size_t          np    = th_func_nparams(callee);
    size_t          given = 0;
    const th_param *rest  = th_func_rest(callee);

    for (uint32_t i = 1; i < in->n_operands; i++) {
        if (in->operands[i].kind == IR_OPD_NAMED_ARG) {
            const th_kwparam *k = th_func_kw_lookup(callee,
                                                    in->operands[i].named.name);

            if (!k) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                         "%s has no keyword parameter %s",
                         typed(m, callee), in->operands[i].named.name);

                return REJECT;
            }

            if (!arg_matches(m, k->mode, k->type, opd_type(in, i))) {
                blame(d,
                      complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                               k->mode == TH_IN
                                   ? "keyword parameter %s is %s, and the "
                                     "argument is %s"
                                   : "keyword parameter %s is %s out or "
                                     "inout, so the argument must be a "
                                     "pointer to it, not %s",
                               in->operands[i].named.name,
                               typed(m, ir_type_intern(m, k->type)),
                               typed(m, opd_type(in, i))),
                      in, i, "the argument is defined here");

                return REJECT;
            }
            continue;
        }

        const th_param *p = given < np ? th_func_param(callee, given) : rest;

        if (p && !arg_matches(m, p->mode, p->type, opd_type(in, i))) {
            blame(d,
                  complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                           p->mode != TH_IN
                               ? "%s parameter %zu is %s out or inout, so "
                                 "the argument must be a pointer to it, not "
                                 "%s"
                               : "%s parameter %zu is %s, and the argument "
                                 "is %s",
                           given < np ? "positional" : "rest",
                           given, typed(m, ir_type_intern(m, p->type)),
                           typed(m, opd_type(in, i))),
                  in, i, "the argument is defined here");

            return REJECT;
        }
        given++;
    }

    if (given < np) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this call",
                 "%s takes %zu positional arguments and %zu were given",
                 typed(m, callee), np, given);

        return REJECT;
    }

    if (given > np && !rest) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this call",
                 "%s takes %zu positional arguments, has no rest parameter, "
                 "and %zu were given", typed(m, callee), np, given);

        return REJECT;
    }

    th_type *ret = th_func_ret(callee);

    return ACCEPT(ret ? ir_type_intern(m, ret) : nullptr);
}

static ir_rule_result_t rule_size_like(ir_module_t *m, ir_instr_t *in,
                                       ir_diags_t *d, void *state)
{
    (void)state;

    th_type *u = ir_profile_scalar(m, IR_CLASS_UINT, 64);

    if (u && in->n_operands && in->operands[0].kind == IR_OPD_TYPE) {
        const ir_descriptor_t *dsc = ir_layout_of(m, in->operands[0].type,
                                                  nullptr);
        const ir_layout_val_t *v   = nullptr;

        if (dsc && in->op == IR_OP_SIZEOF) {
            v = &dsc->size;
        }
        else if (dsc && in->op == IR_OP_ALIGNOF) {
            v = &dsc->align;
        }
        else if (dsc && in->op == IR_OP_OFFSETOF && in->n_operands > 1) {
            for (uint32_t i = 0; i < dsc->n_members; i++) {
                if (!strcmp(dsc->members[i].name, in->operands[1].name)) {
                    v = &dsc->members[i].bit_offset;
                }
            }
        }

        if (v && ir_layout_is_settled(v)) {
            return accept_const(m, u,
                                (ir_annot_t){.kind = IR_ANNOT_INT,
                                             .i = {.mag = v->state
                                                              == IR_LAYOUT_UNSET
                                                              ? 0
                                                              : v->value}});
        }
    }

    if (!u) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "here",
                 "%s yields a 64-bit unsigned integer, and profile %s "
                 "declares no such type", ir_module_op_spelling(m, in->op),
                 m->profile->name);

        return REJECT;
    }

    return ACCEPT(u);
}

static ir_rule_result_t rule_offsetof(ir_module_t *m, ir_instr_t *in,
                                      ir_diags_t *d, void *state)
{
    th_type *rc = record_of(m, opd_type(in, 0));

    if (in->n_operands < 2) {
        return DECLINE;
    }

    if (!rc) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this type",
                 "offsetof needs a record type, and %s is not one",
                 typed(m, opd_type(in, 0)));

        return REJECT;
    }

    if (!th_record_lookup(rc, in->operands[1].name)) {
        complain(m, d, IR_DIAG_TYPE_MEMBER, in, "this name",
                 "%s has no member %s", typed(m, rc), in->operands[1].name);

        return REJECT;
    }

    return rule_size_like(m, in, d, state);
}

static ir_rule_result_t rule_typehash(ir_module_t *m, ir_instr_t *in,
                                      ir_diags_t *d, void *state)
{
    ir_rule_result_t r = rule_size_like(m, in, d, state);
    th_type         *t = opd_type(in, 0);

    if (r.verdict != IR_RULE_ACCEPT || !t) {
        return r;
    }

    return accept_const(m, r.result,
                        (ir_annot_t){
                            .kind = IR_ANNOT_INT,
                            .i = ir_lit_hex(th_hash(ir_ctx_types(m->ctx), t))});
}

static ir_rule_result_t rule_armtag(ir_module_t *m, ir_instr_t *in,
                                    ir_diags_t *d, void *state)
{
    ir_rule_result_t r = rule_size_like(m, in, d, state);

    if (r.verdict != IR_RULE_ACCEPT || !in->n_operands) {
        return r;
    }

    return accept_const(m, r.result,
                        (ir_annot_t){
                            .kind = IR_ANNOT_INT,
                            .i = ir_lit_hex(th_arm_tag(in->operands[0].name))});
}

static ir_rule_result_t rule_typename(ir_module_t *m, ir_instr_t *in,
                                      ir_diags_t *d, void *state)
{
    (void)state;

    th_type *c = ir_profile_role(m, IR_ROLE_CHAR);

    if (!c) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "here",
                 "typename yields a pointer to a character, and profile %s "
                 "declares no character type", m->profile->name);

        return REJECT;
    }

    th_type *p = th_ind(ir_ctx_types(m->ctx),
                        TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE, c);
    th_type *t = opd_type(in, 0);

    if (!t) {
        return DECLINE;
    }

    return accept_const(m, ir_type_intern(m, p),
                        (ir_annot_t){.kind = IR_ANNOT_STR,
                                     .s    = ir_type_text(m, t)});
}

static ir_rule_result_t rule_branch(ir_module_t *m, ir_instr_t *in,
                                    ir_diags_t *d, void *state)
{
    (void)state;

    th_type *c = opd_type(in, 0);

    if (!c) {
        return DECLINE;
    }

    if (ir_machine_of(m, c).class != IR_CLASS_BOOL) {
        blame(d, complain(m, d, IR_DIAG_TYPE_CLASS, in, "this condition",
                           "branch needs a boolean condition, and this one "
                           "is %s", typed(m, c)),
              in, 0, "the condition is defined here");

        return REJECT;
    }

    return ACCEPT(nullptr);
}

static ir_rule_result_t rule_switch(ir_module_t *m, ir_instr_t *in,
                                    ir_diags_t *d, void *state)
{
    (void)state;

    th_type *v = opd_type(in, 0);

    if (!v) {
        return DECLINE;
    }

    ir_class_t c = ir_machine_of(m, v).class;

    if (!is_int(c) && c != IR_CLASS_BOOL) {
        blame(d, complain(m, d, IR_DIAG_TYPE_CLASS, in, "this scrutinee",
                           "switch needs an integer or boolean scrutinee, "
                           "and this one is %s", typed(m, v)),
              in, 0, "the scrutinee is defined here");

        return REJECT;
    }

    size_t    want  = (in->n_operands + 1) * 2;
    size_t    cap   = 0;
    size_t    ucap  = 0;
    ir_lit_t *seen  = ir_grow(m->ctx, nullptr, &cap, want, sizeof(ir_lit_t));
    bool     *used  = ir_grow(m->ctx, nullptr, &ucap, want, sizeof(bool));

    for (uint32_t i = 0; i < in->n_operands; i++) {
        if (in->operands[i].kind != IR_OPD_SWITCH_ARM) {
            continue;
        }

        ir_lit_t val = in->operands[i].arm.value;

        if (!int_fits(val, ir_machine_of(m, v))) {
            char *lit = ir_lit_text(val);

            complain(m, d, IR_DIAG_TYPE_LITERAL, in, "this case",
                     "the case value %s is not representable in %s, the type "
                     "of the scrutinee", lit, typed(m, v));
            ncc_free(lit);

            return REJECT;
        }

        size_t h = (size_t)(val.mag ^ (val.mag >> 64)) * 11400714819323198485u;
        size_t at = h & (cap - 1);

        while (used[at] && !ir_lit_eq(seen[at], val)) {
            at = (at + 1) & (cap - 1);
        }

        if (used[at]) {
            char *lit = ir_lit_text(val);

            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this switch",
                     "two cases both match %s", lit);
            ncc_free(lit);

            return REJECT;
        }
        used[at] = true;
        seen[at] = val;
    }

    return ACCEPT(nullptr);
}

static ir_rule_result_t rule_return(ir_module_t *m, ir_instr_t *in,
                                    ir_diags_t *d, void *state)
{
    (void)state;

    ir_func_t *f   = in->block ? in->block->owner : nullptr;
    th_type   *ret = f ? th_func_ret(f->type) : nullptr;

    if (!ret) {
        return DECLINE;
    }

    bool is_void = ir_machine_of(m, ret).class == IR_CLASS_VOID;

    if (is_void && in->n_operands) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this return",
                 "%s returns %s, so return takes no value", f->name,
                 typed(m, ir_type_intern(m, ret)));

        return REJECT;
    }

    if (!is_void && !in->n_operands) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this return",
                 "%s returns %s, so return takes a value", f->name,
                 typed(m, ir_type_intern(m, ret)));

        return REJECT;
    }

    if (in->n_operands
        && !same(m, ir_type_intern(m, ret), opd_type(in, 0))) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this value",
                           "%s returns %s, and this value is %s", f->name,
                           typed(m, ir_type_intern(m, ret)),
                           typed(m, opd_type(in, 0))),
              in, 0, "the value is defined here");

        return REJECT;
    }

    return ACCEPT(nullptr);
}

static ir_rule_result_t rule_none(ir_module_t *m, ir_instr_t *in,
                                  ir_diags_t *d, void *state)
{
    (void)m;
    (void)in;
    (void)d;
    (void)state;

    return ACCEPT(nullptr);
}

typedef struct {
    ir_op_t         op;
    const char     *name;
    ir_type_rule_fn fn;
} entry;

static const entry DEFAULTS[] = {
    {IR_OP_CONST, "ir.const", rule_annotated},
    {IR_OP_AGGREGATE, "ir.aggregate", rule_annotated},
    {IR_OP_UNDEF, "ir.undef", rule_annotated},
    {IR_OP_COPY, "ir.copy", rule_copy},
    {IR_OP_ALLOCA, "ir.alloca", rule_alloca},
    {IR_OP_LOAD, "ir.load", rule_load},
    {IR_OP_STORE, "ir.store", rule_store},
    {IR_OP_FIELD, "ir.field", rule_field},
    {IR_OP_INDEX, "ir.index", rule_index},
    {IR_OP_OFFSET, "ir.offset", rule_offset},
    {IR_OP_DIFF, "ir.diff", rule_diff},
    {IR_OP_MEMCPY, "ir.memcpy", rule_mem},
    {IR_OP_MEMMOVE, "ir.memmove", rule_mem},
    {IR_OP_MEMSET, "ir.memset", rule_mem},
    {IR_OP_MEMCMP, "ir.memcmp", rule_mem},
    {IR_OP_EXTRACT, "ir.extract", rule_extract},
    {IR_OP_INSERT, "ir.insert", rule_insert},
    {IR_OP_VARIANT_MAKE, "ir.variant.make", rule_variant_make},
    {IR_OP_VARIANT_TAG, "ir.variant.tag", rule_variant_tag},
    {IR_OP_VARIANT_PAYLOAD, "ir.variant.payload", rule_variant_payload},
    {IR_OP_ADD, "ir.add", rule_arith},
    {IR_OP_SUB, "ir.sub", rule_arith},
    {IR_OP_MUL, "ir.mul", rule_arith},
    {IR_OP_DIV, "ir.div", rule_arith},
    {IR_OP_REM, "ir.rem", rule_arith},
    {IR_OP_NEG, "ir.neg", rule_arith},
    {IR_OP_AND, "ir.and", rule_bitwise},
    {IR_OP_OR, "ir.or", rule_bitwise},
    {IR_OP_XOR, "ir.xor", rule_bitwise},
    {IR_OP_NOT, "ir.not", rule_bitwise},
    {IR_OP_SHL, "ir.shl", rule_shift},
    {IR_OP_SHR, "ir.shr", rule_shift},
    {IR_OP_ROL, "ir.rol", rule_shift},
    {IR_OP_ROR, "ir.ror", rule_shift},
    {IR_OP_CLZ, "ir.clz", rule_count},
    {IR_OP_CTZ, "ir.ctz", rule_count},
    {IR_OP_POPCOUNT, "ir.popcount", rule_count},
    {IR_OP_BITREVERSE, "ir.bitreverse", rule_bitwise},
    {IR_OP_BYTESWAP, "ir.byteswap", rule_bitwise},
    {IR_OP_EQ, "ir.eq", rule_compare},
    {IR_OP_NE, "ir.ne", rule_compare},
    {IR_OP_LT, "ir.lt", rule_compare},
    {IR_OP_LE, "ir.le", rule_compare},
    {IR_OP_GT, "ir.gt", rule_compare},
    {IR_OP_GE, "ir.ge", rule_compare},
    {IR_OP_CAST, "ir.cast", rule_cast},
    {IR_OP_BITCAST, "ir.bitcast", rule_cast},
    {IR_OP_SELECT, "ir.select", rule_select},
    {IR_OP_PHI, "ir.phi", rule_phi},
    {IR_OP_CALL, "ir.call", rule_call},
    {IR_OP_SIZEOF, "ir.sizeof", rule_size_like},
    {IR_OP_ALIGNOF, "ir.alignof", rule_size_like},
    {IR_OP_OFFSETOF, "ir.offsetof", rule_offsetof},
    {IR_OP_TYPEHASH, "ir.typehash", rule_typehash},
    {IR_OP_TYPENAME, "ir.typename", rule_typename},
    {IR_OP_ARMTAG, "ir.armtag", rule_armtag},
    {IR_OP_JUMP, "ir.jump", rule_none},
    {IR_OP_BRANCH, "ir.branch", rule_branch},
    {IR_OP_SWITCH, "ir.switch", rule_switch},
    {IR_OP_RETURN, "ir.return", rule_return},
    {IR_OP_UNREACHABLE, "ir.unreachable", rule_none},
};

bool ir_rules_add_defaults(ir_rules_t *rs)
{
    if (!rs) {
        return false;
    }

    for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
        if (!ir_rules_add(rs, (ir_rule_t){.name = DEFAULTS[i].name,
                                          .op = DEFAULTS[i].op,
                                          .match = IR_MATCH_DEFAULT,
                                          .type_rule = DEFAULTS[i].fn})) {
            return false;
        }
    }

    return true;
}

size_t ir_default_rule_count(void)
{
    return sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);
}
