#include <math.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/rule.h"
#include "ir/layout.h"
#include "ir/shape.h"
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
    case IR_OPD_SPREAD:
    case IR_OPD_FRESH:
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

    core_buffer_t *b = core_buffer_empty();
    va_list ap;

    va_start(ap, fmt);
    core_buffer_vprintf(b, fmt, ap);
    va_end(ap);

    char *msg = core_buffer_take(b);

    ir_diag_message(dg, d, msg ? msg : fmt);
    core_free(msg);

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
                          : o->kind == IR_OPD_SPREAD    ? o->value
                          : o->kind == IR_OPD_FRESH     ? o->value
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
    core_buffer_t *b = core_buffer_empty();

    if (!n_ax) {
        core_buffer_printf(b, "%s takes no modifiers here",
                  ir_module_op_spelling(m, in->op));
    }
    else {
        core_buffer_printf(b, "%s takes exactly one ", ir_module_op_spelling(m, in->op));

        for (size_t i = 0; i < n_ax; i++) {
            if (i) {
                core_buffer_puts(b, i + 1 == n_ax ? " and one " : ", one ");
            }
            core_buffer_puts(b, ax[i]->what);
        }
        core_buffer_puts(b, n_ax == 1 ? " modifier" : " modifier of each");
    }

    char *msg = core_buffer_take(b);

    complain(m, d, IR_DIAG_TYPE_MODIFIER, in, "these modifiers", "%s",
             msg ? msg : "wrong modifiers");
    core_free(msg);
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
    size_t byte_len = 0;
    if (ir_instr_const_bytes_hex(in, &byte_len)) {
        (void)byte_len;
        if (in->n_operands) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this byte constant",
                     "a byte constant cannot also have scalar operands");
            return REJECT;
        }
        return ACCEPT(t);
    }

    ir_machine_t k = ir_machine_of(m, t);

    if (k.class == IR_CLASS_OPAQUE) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this constant",
                 "%s has no machine class in profile %s, so it has no "
                 "constants", typed(m, t), m->profile->name);

        return REJECT;
    }

    if (!in->n_operands) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this constant",
                 "const needs either a scalar operand or a byte payload");
        return REJECT;
    }

    if (in->operands[0].kind != IR_OPD_CONST) {
        return DECLINE;
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
        core_free(lit);

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

    if (!rc && th_kind_of(th_resolve(t)) == TH_SEQUENCE) {
        th_type *sequence = th_resolve(t);
        th_type *element = th_seq_elem(sequence);
        uint64_t extent = 0;
        bool has_extent = th_seq_extent(sequence, &extent);

        if (has_extent && extent != in->n_operands) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "these operands",
                     "%s has extent %" PRIu64 " and %u values were given",
                     typed(m, t), extent, in->n_operands);
            return REJECT;
        }

        for (uint32_t i = 0; i < in->n_operands; i++) {
            if (!element || !same(m, ir_type_intern(m, element),
                                  opd_type(in, i))) {
                blame(d,
                      complain(m, d, IR_DIAG_TYPE_MISMATCH, in,
                               "this sequence element",
                               "element %u of %s is %s, and the value given "
                               "is %s", i, typed(m, t), typed(m, element),
                               typed(m, opd_type(in, i))),
                      in, i, "the value is defined here");
                return REJECT;
            }
        }

        return ACCEPT(t);
    }

    if (!rc) {
        th_type *aggregate = th_resolve(t);
        const char *aggregate_name = nullptr;
        size_t aggregate_name_len = 0;
        size_t aggregate_args = 0;
        if (th_named_info(aggregate, &aggregate_name,
                          &aggregate_name_len, nullptr, &aggregate_args)
            && aggregate_name_len == strlen(TH_TUPLE)
            && !memcmp(aggregate_name, TH_TUPLE, aggregate_name_len)) {
            if (in->n_operands != aggregate_args) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in, "these operands",
                         "%s has %zu elements and %u values were given",
                         typed(m, t), aggregate_args, in->n_operands);
                return REJECT;
            }
            for (size_t i = 0; i < aggregate_args; i++) {
                const th_arg *argument = th_named_arg(aggregate, i);
                th_type *element = argument
                    && argument->kind == TH_ARG_TYPE ? argument->type
                                                      : nullptr;
                if (!element || !same(m, ir_type_intern(m, element),
                                      opd_type(in, (uint32_t)i))) {
                    blame(d,
                          complain(m, d, IR_DIAG_TYPE_MISMATCH, in,
                                   "this tuple element",
                                   "element %zu of %s has type %s, but its value has type %s",
                                   i, typed(m, t), typed(m, element),
                                   typed(m, opd_type(in, (uint32_t)i))),
                          in, (uint32_t)i, "the value is defined here");
                    return REJECT;
                }
            }
            return ACCEPT(t);
        }

        ir_aggregate_form_t form;
        if (ir_instr_aggregate_form(in, &form)) {
            size_t nargs = th_named_nargs(aggregate);
            size_t expected = form == IR_AGGREGATE_ELEMENTS ? 1 : 2;
            if (th_kind_of(aggregate) != TH_NAMED || nargs != expected) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in,
                         "this aggregate destination",
                         "the aggregate form requires %zu type arguments, but %s has %zu",
                         expected, typed(m, t), nargs);
                return REJECT;
            }
            if (form == IR_AGGREGATE_PAIRS && (in->n_operands & 1u)) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in, "these operands",
                         "a pair aggregate requires an even number of values");
                return REJECT;
            }
            for (uint32_t i = 0; i < in->n_operands; i++) {
                size_t type_index = form == IR_AGGREGATE_PAIRS ? i & 1u : 0;
                const th_arg *argument = th_named_arg(aggregate,
                                                       type_index);
                th_type *element = argument
                    && argument->kind == TH_ARG_TYPE ? argument->type
                                                       : nullptr;
                if (!element || !same(m, ir_type_intern(m, element),
                                      opd_type(in, i))) {
                    blame(d,
                          complain(m, d, IR_DIAG_TYPE_MISMATCH, in,
                                   "this aggregate element",
                                   "value %u of %s has type %s, not %s", i,
                                   typed(m, t), typed(m, opd_type(in, i)),
                                   typed(m, element)),
                          in, i, "the value is defined here");
                    return REJECT;
                }
            }
            return ACCEPT(t);
        }
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

static ir_rule_result_t rule_rest_collect(ir_module_t *m, ir_instr_t *in,
                                          ir_diags_t *d, void *state)
{
    (void)state;
    ir_value_t *rest = in->n_operands == 1
        && in->operands[0].kind == IR_OPD_VALUE
            ? in->operands[0].value : nullptr;
    th_type *result = declared(in);
    th_type *sequence = th_resolve(result);
    th_type *element = sequence && th_kind_of(sequence) == TH_SEQUENCE
        ? th_seq_elem(sequence) : nullptr;
    if (!rest || !rest->is_rest || !element
        || !same(m, ir_type_intern(m, element), opd_type(in, 0))) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this rest collection",
                 "rest.collect needs a rest parameter and a sequence result "
                 "with the same element type");
        return REJECT;
    }
    return ACCEPT(result);
}

static ir_rule_result_t rule_param_present(ir_module_t *m, ir_instr_t *in,
                                           ir_diags_t *d, void *state)
{
    (void)state;
    ir_value_t *parameter = in->n_operands == 1
        && in->operands[0].kind == IR_OPD_VALUE
            ? in->operands[0].value : nullptr;
    if (!ir_param_is_optional(parameter)) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this parameter presence test",
                 "param.present requires an optional function parameter");
        return REJECT;
    }
    return ACCEPT(ir_type_parse(m, "bool"));
}

static uint32_t sequence_rank(th_type *type)
{
    uint32_t rank = 0;
    th_type *resolved = type ? th_resolve(type) : nullptr;
    while (resolved && th_kind_of(resolved) == TH_SEQUENCE) {
        th_type *element = th_seq_elem(resolved);
        if (!element || rank == UINT32_MAX) return 0;
        resolved = th_resolve(element);
        rank++;
    }
    return rank;
}

static th_type *sequence_element_type(th_type *type)
{
    th_type *resolved = type ? th_resolve(type) : nullptr;
    while (resolved && th_kind_of(resolved) == TH_SEQUENCE) {
        th_type *element = th_seq_elem(resolved);
        if (!element) return nullptr;
        resolved = th_resolve(element);
    }
    return resolved;
}

static ir_rule_result_t rule_array_new(ir_module_t *m, ir_instr_t *in,
                                       ir_diags_t *d, th_type *type)
{
    uint32_t rank = sequence_rank(type);
    th_type *element_type = sequence_element_type(type);
    ir_shape_t *shape = in->dest ? ir_value_shape(in->dest) : nullptr;
    ir_array_init_t initialization;
    ir_shape_storage_t storage = {0};
    if (!ir_instr_array_init(in, &initialization)) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in,
                 "this array element initialization policy",
                 "array.new requires a recognized element initialization policy");
        return REJECT;
    }
    (void)initialization;
    if (!rank || !shape || ir_shape_rank(shape) != rank
        || !ir_shape_storage(shape, &storage)
        || !storage.classification_known) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this array construction",
                 "array.new requires a sequence result with a complete shape");
        return REJECT;
    }
    if (!element_type
        || !same(m, element_type, ir_shape_element_type(shape))) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this array shape",
                 "the shape element type is %s, but the result stores %s",
                 typed(m, ir_shape_element_type(shape)),
                 typed(m, element_type));
        return REJECT;
    }

    uint32_t known_extent = 0;
    uint32_t known_prefix = 0;
    bool unresolved = false;
    for (uint32_t i = 0; i < rank; i++) {
        ir_shape_extent_t extent = {0};
        if (!ir_shape_extent(shape, i, &extent)) return REJECT;
        if (extent.kind == IR_SHAPE_EXTENT_UNRESOLVED) {
            unresolved = true;
            continue;
        }
        if (unresolved) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this array shape",
                     "known extents must precede unresolved dimensions");
            return REJECT;
        }
        known_prefix++;
        uint32_t operand_offset = in->op == IR_OP_ARRAY_NEW_USING ? 1 : 0;
        uint32_t operand_index = operand_offset + known_extent;
        if (!extent.value || operand_index >= in->n_operands
            || in->operands[operand_index].kind != IR_OPD_VALUE
            || in->operands[operand_index].value != extent.value) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in,
                     "these known extents",
                     "array.new operands must match every known dimension in its shape");
            return REJECT;
        }
        ir_machine_t machine = ir_machine_of(m,
            opd_type(in, operand_index));
        if (!is_int(machine.class)) {
            complain(m, d, IR_DIAG_TYPE_CLASS, in,
                     "this array extent",
                     "array.new dimensions must be integers");
            return REJECT;
        }
        known_extent++;
    }
    if (known_extent + (in->op == IR_OP_ARRAY_NEW_USING ? 1u : 0u)
        != in->n_operands) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "these array dimensions",
                 "array.new accepts one operand for each known dimension");
        return REJECT;
    }
    ir_shape_measure_t element_count = {0};
    if (known_prefix != storage.flat_prefix_rank
        || !ir_shape_element_count(shape, &element_count)
        || element_count.kind == IR_SHAPE_MEASURE_UNKNOWN
        || (element_count.kind == IR_SHAPE_MEASURE_RUNTIME
            && !element_count.value)) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this array shape",
                 "array.new requires a flat-prefix classification and element count");
        return REJECT;
    }
    for (uint32_t i = 0; i < storage.flat_prefix_rank; i++) {
        ir_shape_measure_t stride = {0};
        if (!ir_shape_stride(shape, i, &stride)
            || stride.kind == IR_SHAPE_MEASURE_UNKNOWN
            || (stride.kind == IR_SHAPE_MEASURE_RUNTIME && !stride.value)) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this array shape",
                     "array.new requires a stride for each flat dimension");
            return REJECT;
        }
    }
    return ACCEPT(type);
}

static ir_rule_result_t
rule_sequence_shape_check(ir_module_t *m, ir_instr_t *in, ir_diags_t *d,
                          void *state)
{
    (void)state;
    ir_shape_t *expected = in->shape_constraint;
    ir_value_t *value = in->n_operands == 1
        && in->operands[0].kind == IR_OPD_VALUE
            ? in->operands[0].value : nullptr;
    ir_shape_t *actual = value ? ir_value_shape(value) : nullptr;
    uint32_t rank = ir_shape_rank(expected);
    if (!expected || !value || !rank
        || rank != sequence_rank(value->type)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this sequence shape check",
                 "sequence.shape.check requires matching ranked sequence shapes");
        return REJECT;
    }
    if (!same(m, ir_shape_element_type(expected),
              sequence_element_type(value->type))) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "these sequence shapes",
                 "sequence.shape.check requires matching element types");
        return REJECT;
    }
    bool constrained = false;
    for (uint32_t i = 0; i < rank; i++) {
        ir_shape_extent_t extent = {0}, actual_extent = {0};
        if (!ir_shape_extent(expected, i, &extent)) return REJECT;
        if (actual && !ir_shape_extent(actual, i, &actual_extent))
            return REJECT;
        if (extent.kind == IR_SHAPE_EXTENT_UNRESOLVED) continue;
        constrained = true;
        if (!extent.value && extent.kind != IR_SHAPE_EXTENT_STATIC
            && !extent.expression && !extent.producer
            && !extent.producer_identifier) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this expected extent",
                     "a constrained extent needs a value or deferred producer");
            return REJECT;
        }
        if (extent.kind == IR_SHAPE_EXTENT_STATIC
            && actual_extent.kind == IR_SHAPE_EXTENT_STATIC
            && extent.static_value != actual_extent.static_value) {
            complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "these sequence shapes",
                     "statically known extents do not match");
            return REJECT;
        }
    }
    if (!constrained) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this sequence shape check",
                 "sequence.shape.check needs at least one constrained extent");
        return REJECT;
    }
    return ACCEPT(nullptr);
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

    if (in->op == IR_OP_ARRAY_NEW || in->op == IR_OP_ARRAY_NEW_USING) {
        return rule_array_new(m, in, d, t);
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

static ir_rule_result_t rule_address_of(ir_module_t *m, ir_instr_t *in,
                                        ir_diags_t *d, void *state)
{
    (void)state;
    ir_value_t *value = in->n_operands
        ? ir_operand_value(&in->operands[0]) : nullptr;
    if (!value) return DECLINE;
    if (value->kind != IR_VAL_LOCAL && value->kind != IR_VAL_PARAM
        && value->kind != IR_VAL_GLOBAL) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this value",
                 "address.of requires an existing variable binding");
        return REJECT;
    }
    th_type *pointer = th_ind(ir_ctx_types(m->ctx), 0, value->type);
    if (!pointer) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this binding",
                 "could not form an address type for this binding");
        return REJECT;
    }
    return ACCEPT(ir_type_intern(m, pointer));
}

static ir_rule_result_t rule_alloca(ir_module_t *m, ir_instr_t *in,
                                    ir_diags_t *d, void *state)
{
    (void)state;

    th_type *t = opd_type(in, 0);

    if (!t) {
        return DECLINE;
    }

    if (in->op == IR_OP_ALLOC_USING && in->n_operands != 2) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this allocation",
                 "alloc.using requires an allocator value");
        return REJECT;
    }

    if (in->op != IR_OP_ALLOC_USING && in->n_operands > 1
        && !is_int(ir_machine_of(m, opd_type(in, 1)).class)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this count",
                 "an alloca count must be an integer, and this one is %s",
                 typed(m, opd_type(in, 1)));

        return REJECT;
    }

    if ((in->op == IR_OP_ALLOC || in->op == IR_OP_ALLOC_USING)
        && in->dest) {
        th_type *result = in->dest->type;
        th_type *referent = th_ind_referent(result);
        if (!referent) {
            complain(m, d, IR_DIAG_TYPE_POINTER, in, "this result",
                     "%s must produce an indirection to its allocated type",
                     ir_module_op_spelling(m, in->op));
            return REJECT;
        }
        if (!same(m, referent, t)) {
            complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this result",
                     "%s produces an address to %s, but its result refers to %s",
                     ir_module_op_spelling(m, in->op),
                     typed(m, t), typed(m, referent));
            return REJECT;
        }
        return ACCEPT(ir_type_intern(m, result));
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

static ir_rule_result_t rule_dealloc(ir_module_t *m, ir_instr_t *in,
                                     ir_diags_t *d, void *state)
{
    (void)state;

    th_type *address = opd_type(in, 0);
    if (!address) {
        return DECLINE;
    }
    if (th_kind_of(address) != TH_INDIRECTION) {
        complain(m, d, IR_DIAG_TYPE_POINTER, in, "this value",
                 "deallocation requires an address, but this value has type %s",
                 typed(m, address));
        return REJECT;
    }
    return ACCEPT(nullptr);
}

static ir_rule_result_t rule_finalize(ir_module_t *m, ir_instr_t *in,
                                      ir_diags_t *d, void *state)
{
    (void)m;
    (void)d;
    (void)state;

    return opd_type(in, 0) ? ACCEPT(nullptr) : DECLINE;
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

    if (ir_instr_has_mod(in, "atomic")) {
        const char *name = nullptr;
        size_t nargs = 0;
        bool is_variant = true;
        if (th_named_info(to, &name, nullptr, &is_variant, &nargs)
            && !is_variant && nargs == 1 && name
            && !strcmp(name, "atomic")) {
            const th_arg *argument = th_named_arg(to, 0);
            if (argument && argument->kind == TH_ARG_TYPE)
                to = argument->type;
        }
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

    bool matches = same(m, ir_type_intern(m, to), v);
    if (!matches && ir_instr_has_mod(in, "atomic")) {
        const char *name = nullptr;
        size_t nargs = 0;
        bool is_variant = true;
        if (th_named_info(to, &name, nullptr, &is_variant, &nargs)
            && !is_variant && nargs == 1 && name
            && !strcmp(name, "atomic")) {
            const th_arg *argument = th_named_arg(to, 0);
            matches = argument && argument->kind == TH_ARG_TYPE
                && same(m, ir_type_intern(m, argument->type), v);
        }
    }
    if (!matches) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this value",
                           "storing %s through a pointer to %s", typed(m, v),
                           typed(m, ir_type_intern(m, to))),
              in, 1, "the value is defined here");

        return REJECT;
    }

    return ACCEPT(nullptr);
}

static ir_rule_result_t rule_global_store(ir_module_t *m, ir_instr_t *in,
                                          ir_diags_t *d, void *state)
{
    (void)state;
    if (in->n_operands < 2 || in->operands[0].kind != IR_OPD_VALUE
        || !in->operands[0].value
        || in->operands[0].value->kind != IR_VAL_GLOBAL)
        return DECLINE;
    ir_value_t *target = in->operands[0].value;
    ir_global_t *global = ir_global_find(m, target->name);
    th_type *value = opd_type(in, 1);
    if (!global || !value) return DECLINE;
    ir_func_t *owner = in->block ? in->block->owner : nullptr;
    ir_execution_phase_t phase = ir_func_execution_phase(owner);
    bool initializes = ir_instr_is_binding_initialization(in)
        || ir_func_is_initializer(owner);
    bool phase_writable = ir_global_can_write(global, phase)
        || (initializes
            && ir_global_can_initialize(global, phase));
    if (!phase_writable) {
        complain(m, d, IR_DIAG_VERIFY_PHASE, in, "this global",
                 "global %s is neither mutable nor initializable during %s execution",
                 global->name,
                 phase == IR_PHASE_COMPTIME ? "compile-time" : "runtime");
        return REJECT;
    }
    ir_mutability_t mutability =
        phase == IR_PHASE_COMPTIME
            ? ir_global_comptime_mutability(global) : global->mutability;
    if (mutability == IR_MUT_CONST) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this global",
                 ir_func_execution_phase(owner) == IR_PHASE_COMPTIME
                     ? "a constant global is immutable during compile-time execution"
                     : "a constant global cannot be assigned at runtime");
        return REJECT;
    }
    if (!same(m, global->type, value)) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this value",
                           "storing %s in global %s of type %s",
                           typed(m, value), global->name,
                           typed(m, global->type)),
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

        th_type *actual = opd_type(in, 1);
        bool compatible = same(m, ir_type_intern(m, payload), actual);
        th_type *payload_shape = ir_type_definition(m,
                                                    th_resolve(payload));
        if (!compatible && payload_shape && actual
            && th_kind_of(th_resolve(payload_shape)) == TH_RECORD
            && th_kind_of(th_resolve(actual)) == TH_RECORD
            && th_hash(ir_ctx_types(m->ctx), payload_shape)
                == th_hash(ir_ctx_types(m->ctx), actual))
            compatible = true;
        if (!compatible) {
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

static ir_rule_result_t rule_variant_new(ir_module_t *m, ir_instr_t *in,
                                         ir_diags_t *d, void *state)
{
    (void)state;

    th_type *type = declared(in);
    bool is_variant = false;
    if (!type) {
        complain(m, d, IR_DIAG_TYPE_DEST, in, "this instruction",
                 "variant.new takes its type from its destination, and has "
                 "none");
        return REJECT;
    }
    if (!th_named_info(type, nullptr, nullptr, &is_variant, nullptr)
        || !is_variant) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this destination",
                 "variant.new needs a variant type, and %s is not one",
                 typed(m, type));
        return REJECT;
    }
    if (in->op == IR_OP_VARIANT_NEW_USING
        && (!in->n_operands || !opd_type(in, 0))) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this allocator",
                 "variant.new.using requires an allocator value");
        return REJECT;
    }
    return ACCEPT(type);
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

static bool is_option_type(th_type *type, th_type **element)
{
    const char *name = nullptr;
    size_t nargs = 0;
    bool is_variant = true;
    if (!type || !th_named_info(type, &name, nullptr, &is_variant, &nargs)
        || is_variant || nargs != 1 || !name || strcmp(name, "option"))
        return false;
    const th_arg *arg = th_named_arg(type, 0);
    if (!arg || arg->kind != TH_ARG_TYPE || !arg->type) return false;
    if (element) *element = arg->type;
    return true;
}

static ir_rule_result_t rule_option_is_nil(ir_module_t *m, ir_instr_t *in,
                                           ir_diags_t *d, void *state)
{
    (void)state;
    if (!is_option_type(opd_type(in, 0), nullptr)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "option.is_nil requires an option value, and this one is %s",
                 typed(m, opd_type(in, 0)));
        return REJECT;
    }
    return ACCEPT(ir_profile_scalar(m, IR_CLASS_BOOL, 1));
}

static ir_rule_result_t rule_nil(ir_module_t *m, ir_instr_t *in,
                                 ir_diags_t *d, void *state)
{
    (void)state;
    th_type *nil = declared(in);
    if (nil && ir_type_eq(m, nil, th_name(ir_ctx_types(m->ctx), "nil")))
        return ACCEPT(nil);
    complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this result",
             "nil produces the nil sentinel type, not %s", typed(m, nil));
    return REJECT;
}

static ir_rule_result_t rule_option_none(ir_module_t *m, ir_instr_t *in,
                                         ir_diags_t *d, void *state)
{
    (void)state;
    th_type *option = declared(in);
    if (option && is_option_type(option, nullptr)) return ACCEPT(option);
    complain(m, d, IR_DIAG_TYPE_CLASS, in, "this result",
             "option.none must produce an option type, not %s",
             typed(m, option));
    return REJECT;
}

static ir_rule_result_t rule_option_some(ir_module_t *m, ir_instr_t *in,
                                         ir_diags_t *d, void *state)
{
    (void)state;
    th_type *element = nullptr;
    th_type *option = declared(in);
    if (!option || !is_option_type(option, &element)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this result",
                 "option.some must produce an option type, not %s",
                 typed(m, option));
        return REJECT;
    }
    if (!ir_type_eq(m, element, opd_type(in, 0))) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this payload",
                 "option.some payload has type %s; expected %s",
                 typed(m, opd_type(in, 0)), typed(m, element));
        return REJECT;
    }
    return ACCEPT(option);
}

static ir_rule_result_t rule_option_get(ir_module_t *m, ir_instr_t *in,
                                        ir_diags_t *d, void *state)
{
    (void)state;
    th_type *element = nullptr;
    if (!is_option_type(opd_type(in, 0), &element)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "option.get requires an option value, and this one is %s",
                 typed(m, opd_type(in, 0)));
        return REJECT;
    }
    return ACCEPT(element);
}

static ir_rule_result_t rule_option_get_or_else(ir_module_t *m,
                                                ir_instr_t *in,
                                                ir_diags_t *d, void *state)
{
    (void)state;
    th_type *element = nullptr;
    if (!is_option_type(opd_type(in, 0), &element)) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this operand",
                 "option.get_or_else requires an option value, and this one is %s",
                 typed(m, opd_type(in, 0)));
        return REJECT;
    }
    if (ir_type_eq(m, element, opd_type(in, 1)))
        return ACCEPT(element);
    complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this operand",
             "option.get_or_else fallback has type %s; expected %s",
             typed(m, opd_type(in, 1)), typed(m, element));
    return REJECT;
}

static ir_rule_result_t rule_atomic_wrap(ir_module_t *m, ir_instr_t *in,
                                         ir_diags_t *d, void *state)
{
    (void)state;
    th_type *atomic = declared(in);
    const char *name = nullptr;
    size_t nargs = 0;
    bool is_variant = true;
    const th_arg *argument = nullptr;
    if (!atomic || !th_named_info(atomic, &name, nullptr, &is_variant,
                                  &nargs)
        || is_variant || nargs != 1 || !name || strcmp(name, "atomic")
        || !(argument = th_named_arg(atomic, 0))
        || argument->kind != TH_ARG_TYPE) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this result",
                 "atomic.wrap must produce atomic[T], not %s",
                 typed(m, atomic));
        return REJECT;
    }
    if (!same(m, argument->type, opd_type(in, 0))) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this value",
                 "atomic.wrap needs %s, not %s",
                 typed(m, argument->type), typed(m, opd_type(in, 0)));
        return REJECT;
    }
    return ACCEPT(atomic);
}

static bool type_test_source(ir_module_t *m, th_type *source,
                             th_type *target)
{
    if (!source || !target) return false;
    if (ir_type_eq(m, source, target)) return true;
    th_comb target_comb;
    if (th_sum_comb(target, &target_comb) && target_comb == TH_UNION) {
        for (size_t i = 0; i < th_sum_len(target); i++)
            if (!type_test_source(m, source, th_sum_arm(target, i)))
                return false;
        return true;
    }
    th_comb comb;
    if (th_sum_comb(source, &comb) && comb == TH_UNION) {
        for (size_t i = 0; i < th_sum_len(source); i++)
            if (ir_type_eq(m, th_sum_arm(source, i), target)) return true;
        return false;
    }
    const char *name = nullptr;
    size_t length = 0;
    return th_named_info(source, &name, &length, nullptr, nullptr)
        && length == 3 && !memcmp(name, "any", 3);
}

static ir_rule_result_t rule_type_test(ir_module_t *m, ir_instr_t *in,
                                       ir_diags_t *d, void *state)
{
    (void)state;
    th_type *source = opd_type(in, 0);
    th_type *target = opd_type(in, 1);
    if (!source || !target) return DECLINE;
    if (!type_test_source(m, source, target)) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this type test",
                 "%s cannot contain a value of %s", typed(m, source),
                 typed(m, target));
        return REJECT;
    }
    th_type *boolean = ir_profile_scalar(m, IR_CLASS_BOOL, 0);
    if (!boolean) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "here",
                 "type.test yields bool, which profile %s does not define",
                 m->profile->name);
        return REJECT;
    }
    return ACCEPT(boolean);
}

static ir_rule_result_t rule_type_narrow(ir_module_t *m, ir_instr_t *in,
                                         ir_diags_t *d, void *state)
{
    (void)state;
    th_type *source = opd_type(in, 0);
    th_type *target = opd_type(in, 1);
    if (!source || !target) return DECLINE;
    if (!type_test_source(m, source, target)) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this narrowing",
                 "%s cannot be narrowed to %s", typed(m, source),
                 typed(m, target));
        return REJECT;
    }
    if (!ir_type_eq(m, declared(in), target)) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this destination",
                 "type.narrow targets %s, and its destination is %s",
                 typed(m, target), typed(m, declared(in)));
        return REJECT;
    }
    return ACCEPT(target);
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

    bool signedness_change =
        (f.class == IR_CLASS_SINT && t.class == IR_CLASS_UINT)
        || (f.class == IR_CLASS_UINT && t.class == IR_CLASS_SINT);
    if (f2i || (i2i && (t.width < f.width || signedness_change))) {
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

static bool call_arg_matches(ir_module_t *m, th_mode mode, th_type *want,
                             th_type *got, bool instantiate)
{
    if (!want || !got) return false;
    (void)mode;
    return same(m, ir_type_intern(m, want), ir_type_intern(m, got))
        || (instantiate
            ? th_unify_in_place(ir_ctx_types(m->ctx), want, got)
            : th_unify(ir_ctx_types(m->ctx), want, got)) != nullptr;
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

    uint64_t contract_arity = 0;
    bool generic_contract = callee && th_kind_of(callee) == TH_BINDER
        && th_binder_arity(callee, &contract_arity) && contract_arity > 0;
    if (callee && th_kind_of(callee) == TH_BINDER && !generic_contract)
        callee = th_binder_body(callee);
    if (generic_contract) {
        th_type *instance = th_instantiate(ir_ctx_types(m->ctx), callee);
        th_type *call_signature = ir_instr_call_signature(in);
        if (!instance) return DECLINE;
        if (!call_signature) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this call",
                     "a generic call requires its instantiated call signature");
            return REJECT;
        }
        if (!th_unify(ir_ctx_types(m->ctx), instance, call_signature)) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this call",
                     "the call-site signature is not an instance of the callee contract");
            return REJECT;
        }
        callee = call_signature;
    }

    if (!callee || th_kind_of(callee) != TH_FUNCTION) {
        complain(m, d, IR_DIAG_TYPE_CLASS, in, "this callee",
                 "call needs a function or a pointer to one, and this is %s",
                 typed(m, opd_type(in, 0)));

        return REJECT;
    }

    th_type *outcome_type = ir_call_outcome_type(m, callee);
    if (!outcome_type) return DECLINE;
    th_type *failure = th_func_error(callee);
    bool has_failure = failure
        && ir_machine_of(m, failure).class != IR_CLASS_VOID;
    if (!generic_contract && has_failure
        && !same(m, declared(in), outcome_type)) {
        complain(m, d, IR_DIAG_VERIFY_TYPED, in, "this call",
                 "a call with an error channel must produce its tagged outcome type %s",
                 typed(m, outcome_type));
        return REJECT;
    }

    size_t          np    = th_func_nparams(callee);
    size_t          given = 0;
    const th_param *rest  = th_func_rest(callee);
    ir_func_t      *callee_function = in->n_operands
        && in->operands[0].kind == IR_OPD_VALUE
        && in->operands[0].value
        && in->operands[0].value->kind == IR_VAL_FUNC
            ? in->operands[0].value->owner : nullptr;
    bool spread_seen = false;

    for (uint32_t i = 1; i < in->n_operands; i++) {
        if (in->operands[i].kind == IR_OPD_MISSING) {
            const th_param *parameter = given < np
                ? th_func_param(callee, given) : nullptr;
            bool optional = callee_function
                && ir_func_parameter_is_optional(callee_function, given);
            if (!parameter || parameter->mode != TH_IN || !optional) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in,
                         "this omitted argument",
                         "an omitted call argument must match an optional "
                         "input parameter on a directly known function");
                return REJECT;
            }
            given++;
            continue;
        }
        if (in->operands[i].kind == IR_OPD_OUT) {
            const th_param *parameter = given < np
                ? th_func_param(callee, given) : nullptr;
            if (!parameter || parameter->mode != TH_OUT) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in, "this out argument",
                         "an out placeholder must match an out parameter");
                return REJECT;
            }
            given++;
            continue;
        }
        if (in->operands[i].kind == IR_OPD_FRESH) {
            const th_param *parameter = given < np
                ? th_func_param(callee, given) : nullptr;
            th_type *storage_type = opd_type(in, i);
            th_type *referent = storage_type
                ? th_ind_referent(storage_type) : nullptr;
            if (!parameter || parameter->mode != TH_INOUT || !referent
                || !same(m, ir_type_intern(m, parameter->type),
                         ir_type_intern(m, referent))) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in,
                         "this fresh construction target",
                         "fresh storage must match a modify parameter");
                return REJECT;
            }
            given++;
            continue;
        }
        if (in->operands[i].kind == IR_OPD_SPREAD) {
            ir_value_t *spread = in->operands[i].value;
            if (!rest || !spread || !spread->is_rest || given < np ||
                spread_seen || !call_arg_matches(m, rest->mode, rest->type,
                                                 opd_type(in, i),
                                                 generic_contract)) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in, "this spread argument",
                         "a spread argument must match the callee's rest "
                         "parameter and follow all fixed positional arguments");
                return REJECT;
            }
            spread_seen = true;
            continue;
        }
        if (in->operands[i].kind == IR_OPD_NAMED_ARG) {
            const char *name = in->operands[i].named.name;
            if (!name) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                         "a keyword argument must have a name");
                return REJECT;
            }
            for (uint32_t j = 1; j < i; j++) {
                if (in->operands[j].kind == IR_OPD_NAMED_ARG &&
                    in->operands[j].named.name &&
                    !strcmp(name, in->operands[j].named.name)) {
                    complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                             "keyword parameter %s is supplied more than once",
                             name);
                    return REJECT;
                }
            }
            const th_kwparam *k = th_func_kw_lookup(callee,
                                                    name);

            if (!k) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                         "%s has no keyword parameter %s",
                         typed(m, callee), in->operands[i].named.name);

                return REJECT;
            }

            bool is_out = in->operands[i].named.value == nullptr;
            if (is_out != (k->mode == TH_OUT)) {
                complain(m, d, IR_DIAG_TYPE_ARGS, in, "this keyword argument",
                         "keyword parameter %s must be supplied as %s",
                         name, k->mode == TH_OUT ? "out" : "a value");
                return REJECT;
            }
            if (is_out) continue;

            if (!call_arg_matches(m, k->mode, k->type, opd_type(in, i),
                                  generic_contract)) {
                blame(d,
                      complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                               "keyword parameter %s is %s, and the "
                               "argument is %s",
                               in->operands[i].named.name,
                               typed(m, ir_type_intern(m, k->type)),
                               typed(m, opd_type(in, i))),
                      in, i, "the argument is defined here");

                return REJECT;
            }
            continue;
        }

        if (spread_seen) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                     "positional arguments cannot follow a spread argument");
            return REJECT;
        }

        const th_param *p = given < np ? th_func_param(callee, given) : rest;

        if (p && p->mode == TH_OUT) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                     "out parameter %zu needs an out placeholder, not an input value",
                     given);
            return REJECT;
        }
        if (p && !call_arg_matches(m, p->mode, p->type, opd_type(in, i),
                                   generic_contract)) {
            blame(d,
                  complain(m, d, IR_DIAG_TYPE_ARGS, in, "this argument",
                           "%s parameter %zu is %s, and the argument "
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

    if (generic_contract && !same(m, declared(in), outcome_type)) {
        if (!th_unify_in_place(ir_ctx_types(m->ctx), outcome_type,
                               declared(in))) {
            complain(m, d, IR_DIAG_VERIFY_TYPED, in, "this call",
                     "the call result does not match the instantiated return type %s",
                     typed(m, outcome_type));
            return REJECT;
        }
        return ACCEPT(declared(in));
    }
    if (has_failure && !same(m, declared(in), outcome_type)) {
        complain(m, d, IR_DIAG_VERIFY_TYPED, in, "this call",
                 "a call with an error channel must produce its tagged outcome type %s",
                 typed(m, outcome_type));
        return REJECT;
    }
    return ACCEPT(outcome_type);
}

static ir_rule_result_t rule_call_output(ir_module_t *m, ir_instr_t *in,
                                         ir_diags_t *d, void *state)
{
    (void)state;
    ir_value_t *source = in->n_operands > 0
        ? ir_operand_value(&in->operands[0]) : nullptr;
    if (!source || !source->def || in->n_operands != 2
        || in->operands[1].kind != IR_OPD_CONST
        || in->operands[1].lit.is_float) return DECLINE;

    ir_instr_t *call = source->def;
    if (call->op == IR_OP_VARIANT_PAYLOAD) {
        if (call->n_operands != 2
            || call->operands[1].kind != IR_OPD_NAME
            || strcmp(call->operands[1].name, "ok")) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this value",
                     "copy-out values are available only from a successful call result");
            return REJECT;
        }
        ir_value_t *outcome = ir_operand_value(&call->operands[0]);
        call = outcome ? outcome->def : nullptr;
        if (!call || call->op != IR_OP_CALL) return DECLINE;
    }
    else if (call->op != IR_OP_CALL) {
        return DECLINE;
    }

    th_type *signature = ir_instr_call_signature(call);
    if (!signature) signature = opd_type(call, 0);
    signature = signature ? th_resolve(signature) : nullptr;
    if (signature && th_kind_of(signature) == TH_INDIRECTION)
        signature = th_ind_referent(signature);
    if (signature && th_kind_of(signature) == TH_BINDER)
        signature = th_binder_body(signature);
    if (!signature || th_kind_of(signature) != TH_FUNCTION) return DECLINE;
    if (call->op == IR_OP_CALL) {
        th_type *error = th_func_error(signature);
        bool fallible = error
            && ir_machine_of(m, error).class != IR_CLASS_VOID;
        if (fallible && source->def->op != IR_OP_VARIANT_PAYLOAD) {
            complain(m, d, IR_DIAG_TYPE_ARGS, in, "this call result",
                     "a fallible call's copy-out values are available only on its successful path");
            return REJECT;
        }
    }

    ir_lit_t literal = in->operands[1].lit.i;
    if (literal.neg || literal.mag > SIZE_MAX) return DECLINE;
    th_type *output = ir_call_output_type(signature,
                                           (size_t)literal.mag);
    if (!output) {
        complain(m, d, IR_DIAG_TYPE_ARGS, in, "this output index",
                 "the call has no copy-out parameter at index %llu",
                 (unsigned long long)literal.mag);
        return REJECT;
    }
    return ACCEPT(ir_type_intern(m, output));
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

                    break;
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
            core_free(lit);

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
            core_free(lit);

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

    size_t output_count = 0;
    for (size_t i = 0; i < th_func_nparams(f->type); i++) {
        const th_param *parameter = th_func_param(f->type, i);
        if (parameter && parameter->mode != TH_IN) output_count++;
    }
    for (size_t i = 0; i < th_func_nkw(f->type); i++) {
        const th_kwparam *parameter = th_func_kw(f->type, i);
        if (parameter && parameter->mode != TH_IN) output_count++;
    }
    const th_param *rest = th_func_rest(f->type);
    if (rest && rest->mode != TH_IN) output_count++;
    uint32_t result_offset = is_void ? 0 : 1;
    if (in->n_operands != result_offset + output_count) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this return",
                 "%s returns one value for each out or modify parameter",
                 f->name);
        return REJECT;
    }

    if (!is_void && !same(m, ir_type_intern(m, ret), opd_type(in, 0))) {
        blame(d, complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this value",
                           "%s returns %s, and this value is %s", f->name,
                           typed(m, ir_type_intern(m, ret)),
                           typed(m, opd_type(in, 0))),
              in, 0, "the value is defined here");

        return REJECT;
    }

    uint32_t output_index = result_offset;
    for (size_t i = 0; i < th_func_nparams(f->type); i++) {
        const th_param *parameter = th_func_param(f->type, i);
        if (!parameter || parameter->mode == TH_IN) continue;
        if (!same(m, ir_type_intern(m, parameter->type),
                  opd_type(in, output_index))) {
            complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this copy-out value",
                     "copy-out value %u has type %s, expected %s",
                     output_index - result_offset,
                     typed(m, opd_type(in, output_index)),
                     typed(m, ir_type_intern(m, parameter->type)));
            return REJECT;
        }
        output_index++;
    }
    for (size_t i = 0; i < th_func_nkw(f->type); i++) {
        const th_kwparam *parameter = th_func_kw(f->type, i);
        if (!parameter || parameter->mode == TH_IN) continue;
        if (!same(m, ir_type_intern(m, parameter->type),
                  opd_type(in, output_index))) {
            complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this copy-out value",
                     "copy-out value %u has type %s, expected %s",
                     output_index - result_offset,
                     typed(m, opd_type(in, output_index)),
                     typed(m, ir_type_intern(m, parameter->type)));
            return REJECT;
        }
        output_index++;
    }
    if (rest && rest->mode != TH_IN
        && !same(m, ir_type_intern(m, rest->type),
                 opd_type(in, output_index))) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this copy-out value",
                 "the rest copy-out value has the wrong type");
        return REJECT;
    }

    return ACCEPT(nullptr);
}

static ir_rule_result_t rule_fail(ir_module_t *m, ir_instr_t *in,
                                  ir_diags_t *d, void *state)
{
    (void)state;
    ir_func_t *function = in->block ? in->block->owner : nullptr;
    th_type *error = function ? th_func_error(function->type) : nullptr;
    if (!error || ir_machine_of(m, error).class == IR_CLASS_VOID) {
        complain(m, d, IR_DIAG_VERIFY_TYPED, in, "this failure exit",
                 "%s has no non-void failure channel",
                 function ? function->name : "this function");
        return REJECT;
    }
    if (in->n_operands != 1
        || !same(m, ir_type_intern(m, error), opd_type(in, 0))) {
        complain(m, d, IR_DIAG_TYPE_MISMATCH, in, "this failure value",
                 "%s fails with %s, but this value is %s",
                 function->name, typed(m, ir_type_intern(m, error)),
                 in->n_operands ? typed(m, opd_type(in, 0)) : "no value");
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
    {IR_OP_REST_COLLECT, "ir.rest.collect", rule_rest_collect},
    {IR_OP_PARAM_PRESENT, "ir.param.present", rule_param_present},
    {IR_OP_ARRAY_NEW, "ir.array.new", rule_annotated},
    {IR_OP_ARRAY_NEW_USING, "ir.array.new.using", rule_annotated},
    {IR_OP_SEQUENCE_SHAPE_CHECK, "ir.sequence.shape.check",
     rule_sequence_shape_check},
    {IR_OP_UNDEF, "ir.undef", rule_annotated},
    {IR_OP_COPY, "ir.copy", rule_copy},
    {IR_OP_ADDRESS_OF, "ir.address.of", rule_address_of},
    {IR_OP_ALLOCA, "ir.alloca", rule_alloca},
    {IR_OP_ALLOC, "ir.alloc", rule_alloca},
    {IR_OP_ALLOC_USING, "ir.alloc.using", rule_alloca},
    {IR_OP_FINALIZE, "ir.finalize", rule_finalize},
    {IR_OP_DEALLOC, "ir.dealloc", rule_dealloc},
    {IR_OP_LOAD, "ir.load", rule_load},
    {IR_OP_STORE, "ir.store", rule_store},
    {IR_OP_GLOBAL_STORE, "ir.global.store", rule_global_store},
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
    {IR_OP_VARIANT_NEW, "ir.variant.new", rule_variant_new},
    {IR_OP_VARIANT_NEW_USING, "ir.variant.new.using", rule_variant_new},
    {IR_OP_VARIANT_MAKE, "ir.variant.make", rule_variant_make},
    {IR_OP_VARIANT_TAG, "ir.variant.tag", rule_variant_tag},
    {IR_OP_VARIANT_PAYLOAD, "ir.variant.payload", rule_variant_payload},
    {IR_OP_OPTION_IS_NIL, "ir.option.is_nil", rule_option_is_nil},
    {IR_OP_NIL, "ir.nil", rule_nil},
    {IR_OP_OPTION_NONE, "ir.option.none", rule_option_none},
    {IR_OP_OPTION_SOME, "ir.option.some", rule_option_some},
    {IR_OP_OPTION_GET, "ir.option.get", rule_option_get},
    {IR_OP_OPTION_GET_OR_ELSE, "ir.option.get_or_else", rule_option_get_or_else},
    {IR_OP_ATOMIC_WRAP, "ir.atomic.wrap", rule_atomic_wrap},
    {IR_OP_TYPE_TEST, "ir.type.test", rule_type_test},
    {IR_OP_TYPE_NARROW, "ir.type.narrow", rule_type_narrow},
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
    {IR_OP_CALL_OUTPUT, "ir.call-output", rule_call_output},
    {IR_OP_SIZEOF, "ir.sizeof", rule_size_like},
    {IR_OP_ALIGNOF, "ir.alignof", rule_size_like},
    {IR_OP_OFFSETOF, "ir.offsetof", rule_offsetof},
    {IR_OP_TYPEHASH, "ir.typehash", rule_typehash},
    {IR_OP_TYPENAME, "ir.typename", rule_typename},
    {IR_OP_ARMTAG, "ir.armtag", rule_armtag},
    {IR_OP_PHASE_RESULT, "ir.phase_result", rule_annotated},
    {IR_OP_JUMP, "ir.jump", rule_none},
    {IR_OP_BRANCH, "ir.branch", rule_branch},
    {IR_OP_SWITCH, "ir.switch", rule_switch},
    {IR_OP_RETURN, "ir.return", rule_return},
    {IR_OP_FAIL, "ir.fail", rule_fail},
    {IR_OP_TRAP, "ir.trap", rule_none},
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
