#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/rule.h"
#include "ir/text.h"

struct ir_rules_t {
    ir_ctx_t  *ctx;
    ir_rule_t *v;
    size_t     n;
    size_t     cap;
    uint64_t  *keys;
    size_t    *order;
    uint32_t  *op_off;
    uint32_t   n_ops;
    bool       keys_ready;
};

const char *ir_match_name(ir_match_t m)
{
    switch (m) {
    case IR_MATCH_EXACT:
        return "exact type";
    case IR_MATCH_CONSTRUCTOR:
        return "constructor";
    case IR_MATCH_KIND:
        return "type kind";
    case IR_MATCH_CLASS:
        return "machine class";
    default:
        return "opcode default";
    }
}

ir_rules_t *ir_module_rules(ir_module_t *m)
{
    if (!m) {
        return nullptr;
    }

    if (!m->rules) {
        ir_rules_t *rs = ir_rules_from_profile(m->ctx, m->profile);

        if (rs && !ir_rules_add_defaults(rs)) {
            return nullptr;
        }
        m->rules = rs;
    }

    return m->rules;
}

void ir_module_set_rules(ir_module_t *m, ir_rules_t *rs)
{
    if (m) {
        m->rules = rs;
    }
}

ir_rules_t *ir_rules_new(ir_ctx_t *ctx)
{
    if (!ctx) {
        return nullptr;
    }

    ir_rules_t *rs = ir_alloc(ctx, sizeof(ir_rules_t));

    rs->ctx = ctx;

    return rs;
}

ir_rules_t *ir_rules_from_profile(ir_ctx_t *ctx, const ir_profile_t *p)
{
    ir_rules_t *rs = ir_rules_new(ctx);

    if (!rs || !p) {
        return rs;
    }

    for (size_t i = 0; i < p->n_rules; i++) {
        if (!ir_rules_add(rs, p->rules[i])) {
            return nullptr;
        }
    }

    return rs;
}

bool ir_rules_add(ir_rules_t *rs, ir_rule_t rule)
{
    if (!rs || !rule.name) {
        return false;
    }

    if (!rule.type_rule && !rule.lower_rule) {
        return false;
    }

    if ((rule.match == IR_MATCH_EXACT || rule.match == IR_MATCH_CONSTRUCTOR)
        && !rule.type_text) {
        return false;
    }

    rs->v = ir_grow(rs->ctx, rs->v, &rs->cap, rs->n + 1, sizeof(ir_rule_t));

    if (!rs->v) {
        return false;
    }

    rule.name      = ir_strdup(rs->ctx, rule.name);
    rule.type_text = rule.type_text ? ir_strdup(rs->ctx, rule.type_text)
                                    : nullptr;
    rs->v[rs->n++] = rule;
    rs->keys_ready = false;

    return true;
}

size_t ir_rules_count(const ir_rules_t *rs)
{
    return rs ? rs->n : 0;
}

th_type *ir_instr_key_type(ir_instr_t *in)
{
    if (!in) {
        return nullptr;
    }

    for (uint32_t i = 0; i < in->n_operands; i++) {
        const ir_operand_t *o = &in->operands[i];

        if (o->kind == IR_OPD_VALUE && o->value) {
            return o->value->type;
        }

        if (o->kind == IR_OPD_NAMED_ARG && o->named.value) {
            return o->named.value->type;
        }

        if (o->kind == IR_OPD_PHI_ARM && o->phi.value) {
            return o->phi.value->type;
        }

        if (o->kind == IR_OPD_TYPE) {
            return o->type;
        }
    }

    if (in->dest_kind == IR_DEST_VALUE && in->dest) {
        return in->dest->type;
    }

    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local) {
        return in->dest_local->type;
    }

    return nullptr;
}

static const ir_match_t LADDER[] = {
    IR_MATCH_EXACT, IR_MATCH_CONSTRUCTOR, IR_MATCH_KIND, IR_MATCH_CLASS,
    IR_MATCH_DEFAULT,
};

static void ready_keys(ir_module_t *m, ir_rules_t *rs)
{
    if (rs->keys_ready) {
        return;
    }

    rs->keys  = ir_alloc(rs->ctx, (rs->n ? rs->n : 1) * sizeof(uint64_t));
    rs->order = ir_alloc(rs->ctx, (rs->n ? rs->n : 1) * sizeof(size_t));

    for (size_t i = 0; i < rs->n; i++) {
        rs->keys[i] = 0;

        if (rs->v[i].match != IR_MATCH_EXACT || !rs->v[i].type_text) {
            continue;
        }

        th_type *t = ir_type_parse(m, rs->v[i].type_text);

        rs->keys[i] = t ? ir_type_id(m, t) : 0;
    }

    uint32_t top = 0;

    for (size_t i = 0; i < rs->n; i++) {
        top = rs->v[i].op + 1u > top ? rs->v[i].op + 1u : top;
    }
    rs->n_ops  = top;
    rs->op_off = ir_alloc(rs->ctx, (top + 2) * sizeof(uint32_t));

    for (size_t i = 0; i < rs->n; i++) {
        rs->op_off[rs->v[i].op + 1]++;
    }

    for (uint32_t i = 0; i < top; i++) {
        rs->op_off[i + 1] += rs->op_off[i];
    }

    uint32_t *fill = ir_alloc(rs->ctx, (top + 2) * sizeof(uint32_t));

    memcpy(fill, rs->op_off, (top + 2) * sizeof(uint32_t));

    for (size_t rung = 0; rung < sizeof(LADDER) / sizeof(LADDER[0]); rung++) {
        for (size_t i = 0; i < rs->n; i++) {
            if (rs->v[i].match == LADDER[rung]) {
                rs->order[fill[rs->v[i].op]++] = i;
            }
        }
    }
    rs->keys_ready = true;
}

static bool constructor_is(ir_module_t *m, th_type *t, const char *name)
{
    const char *n   = nullptr;
    size_t      len = 0;

    (void)m;

    if (!th_named_info(t, &n, &len, nullptr, nullptr)) {
        return false;
    }

    return strlen(name) == len && !memcmp(n, name, len);
}

static bool rule_matches(ir_module_t *m, ir_rules_t *rs, size_t i,
                         ir_instr_t *in, th_type *key, uint64_t key_id)
{
    const ir_rule_t *r = &rs->v[i];

    if (r->op != in->op) {
        return false;
    }

    switch (r->match) {
    case IR_MATCH_EXACT:
        return key && rs->keys[i] && key_id == rs->keys[i];

    case IR_MATCH_CONSTRUCTOR:
        return key && constructor_is(m, key, r->type_text);

    case IR_MATCH_KIND:
        return key && th_kind_of(key) == r->kind;

    case IR_MATCH_CLASS:
        return key && ir_machine_of(m, key).class == r->class;

    default:
        return true;
    }
}

const ir_rule_t *ir_rule_resolve(ir_module_t *m, ir_rules_t *rs,
                                 ir_instr_t *in, ir_match_t *how)
{
    if (!m || !rs || !in) {
        return nullptr;
    }
    ready_keys(m, rs);

    th_type *key = ir_instr_key_type(in);

    if (in->op >= rs->n_ops) {
        return nullptr;
    }

    uint64_t key_id = key ? ir_type_id(m, key) : 0;

    for (uint32_t k = rs->op_off[in->op]; k < rs->op_off[in->op + 1]; k++) {
        size_t i = rs->order[k];

        if (rule_matches(m, rs, i, in, key, key_id)) {
            if (how) {
                *how = rs->v[i].match;
            }

            return &rs->v[i];
        }
    }

    return nullptr;
}

static th_type *dest_type(ir_instr_t *in)
{
    if (in->dest_kind == IR_DEST_VALUE && in->dest) {
        return in->dest->type;
    }

    if (in->dest_kind == IR_DEST_LOCAL && in->dest_local) {
        return in->dest_local->type;
    }

    return nullptr;
}

#define BOXED_BITS ((uint32_t)(sizeof(ir_uint_t) * CHAR_BIT))

static ir_uint_t boxed_operands(ir_module_t *m, ir_instr_t *in, uint32_t win)
{
    ir_uint_t mask = 0;
    uint32_t  from = win * BOXED_BITS;
    uint32_t  to   = from + BOXED_BITS < in->n_operands ? from + BOXED_BITS
                                                        : in->n_operands;

    for (uint32_t i = from; i < to; i++) {
        th_type *t = nullptr;

        switch (in->operands[i].kind) {
        case IR_OPD_VALUE:
            t = in->operands[i].value ? in->operands[i].value->type : nullptr;
            break;
        case IR_OPD_NAMED_ARG:
            t = in->operands[i].named.value ? in->operands[i].named.value->type
                                            : nullptr;
            break;
        case IR_OPD_PHI_ARM:
            t = in->operands[i].phi.value ? in->operands[i].phi.value->type
                                          : nullptr;
            break;
        default:
            break;
        }

        if (t && !th_is_closed(ir_ctx_types(m->ctx), t)) {
            mask |= (ir_uint_t)1 << (i - from);
        }
    }

    return mask;
}

static void kept_authored(ir_diags_t *d, ir_instr_t *in, const char *key,
                          const char *rule)
{
    ir_diag_t *dg = ir_diag_begin_at(d, IR_DIAG_AUTHORED_ANNOT, in, in->block,
                                     in->block ? in->block->owner : nullptr,
                                     "this instruction");

    if (!dg) {
        return;
    }

    ncc_buffer_t *b = ncc_buffer_empty();

    ncc_buffer_printf(b, "%s would have written %s, which is already there and was "
                  "authored, so the authored one stands", rule, key);

    char *msg = ncc_buffer_take(b);

    ir_diag_message(dg, d, msg);
    ncc_free(msg);
}

static void set_derived(ir_module_t *m, ir_diags_t *d, ir_instr_t *in,
                        ir_annot_t a, const char *rule)
{
    if (!ir_annot_set(m->ctx, &in->annots, a) && a.derived_from
        && ir_annot_get(&in->annots, a.key)) {
        kept_authored(d, in, a.key, rule);
    }
}

static void record_accepted(ir_module_t *m, ir_diags_t *d, ir_instr_t *in,
                            const char *name, ir_rule_result_t *r)
{
    set_derived(m, d, in,
                (ir_annot_t){.key          = "ir.rule",
                             .kind         = IR_ANNOT_STR,
                             .s            = name,
                             .derived_from = IR_PROP_TYPED},
                name);

    uint32_t wins = (in->n_operands + BOXED_BITS - 1) / BOXED_BITS;

    for (uint32_t w = 0; w < wins; w++) {
        ir_uint_t boxed = boxed_operands(m, in, w);
        char      key[32];

        if (!boxed) {
            continue;
        }

        if (w) {
            snprintf(key, sizeof key, "ir.boxed.%u", w);
        }
        else {
            snprintf(key, sizeof key, "ir.boxed");
        }
        set_derived(m, d, in,
                    (ir_annot_t){.key          = key,
                                 .kind         = IR_ANNOT_INT,
                                 .i            = {.mag = boxed},
                                 .derived_from = IR_PROP_TYPED},
                    name);
    }

    for (uint32_t k = 0; k < r->n_annots; k++) {
        set_derived(m, d, in, r->annots[k], name);
    }
}

ir_rule_result_t ir_rule_apply(ir_module_t *m, ir_rules_t *rs, ir_instr_t *in,
                               ir_diags_t *d)
{
    ir_rule_result_t out = {.verdict = IR_RULE_DECLINE};

    if (!m || !rs || !in) {
        return out;
    }
    ready_keys(m, rs);

    th_type *key    = ir_instr_key_type(in);
    uint64_t key_id = key ? ir_type_id(m, key) : 0;

    if (in->op >= rs->n_ops) {
        return (ir_rule_result_t){.verdict = IR_RULE_DECLINE};
    }

    for (uint32_t k = rs->op_off[in->op]; k < rs->op_off[in->op + 1]; k++) {
        size_t i = rs->order[k];

        if (!rs->v[i].type_rule
            || !rule_matches(m, rs, i, in, key, key_id)) {
            continue;
        }

        ir_rule_result_t r = rs->v[i].type_rule(m, in, d, rs->v[i].state);

        if (r.verdict == IR_RULE_DECLINE) {
            continue;
        }

        if (r.verdict == IR_RULE_ACCEPT) {
            th_type *want = dest_type(in);

            if (want && r.result && !ir_type_eq(m, want, r.result)) {
                ir_diag_t *dg = ir_diag_begin_at(d, IR_DIAG_TYPE_DEST, in,
                                                 in->block,
                                                 in->block ? in->block->owner
                                                           : nullptr,
                                                 "destination type");

                if (dg) {
                    ncc_buffer_t *b = ncc_buffer_empty();

                    ncc_buffer_printf(b,
                              "the destination is %s and the result is %s",
                              ir_type_text(m, want),
                              ir_type_text(m, r.result));

                    char *msg = ncc_buffer_take(b);

                    ir_diag_message(dg, d, msg);
                    ncc_free(msg);
                }

                return (ir_rule_result_t){.verdict = IR_RULE_REJECT};
            }

            record_accepted(m, d, in, rs->v[i].name, &r);
        }

        return r;
    }

    return out;
}

bool ir_rule_lower(ir_module_t *m, ir_rules_t *rs, ir_instr_t *in,
                   ir_diags_t *d)
{
    if (!m || !rs || !in) {
        return false;
    }
    ready_keys(m, rs);

    th_type *key    = ir_instr_key_type(in);
    uint64_t key_id = key ? ir_type_id(m, key) : 0;

    if (in->op >= rs->n_ops) {
        return false;
    }

    for (uint32_t k = rs->op_off[in->op]; k < rs->op_off[in->op + 1]; k++) {
        size_t i = rs->order[k];

        if (!rs->v[i].lower_rule
            || !rule_matches(m, rs, i, in, key, key_id)) {
            continue;
        }

        if (rs->v[i].lower_rule(m, in, d, rs->v[i].state)) {
            return true;
        }
    }

    return false;
}

char *ir_rule_explain(ir_module_t *m, ir_rules_t *rs, ir_instr_t *in)
{
    ncc_buffer_t *b = ncc_buffer_empty();

    if (!m || !rs || !in) {
        ncc_buffer_puts(b, "no instruction\n");

        return ncc_buffer_take(b);
    }

    th_type *key = ir_instr_key_type(in);

    ncc_buffer_printf(b, "opcode %s\n", ir_op_spelling(in->op));
    ncc_buffer_puts(b, "key type ");

    if (key) {
        ncc_buffer_puts(b, ir_type_text(m, key));
    }
    else {
        ncc_buffer_puts(b, "none");
    }
    ncc_buffer_puts(b, "\n");

    ready_keys(m, rs);

    for (size_t rung = 0; rung < sizeof(LADDER) / sizeof(LADDER[0]); rung++) {
        ncc_buffer_printf(b, "  %-16s", ir_match_name(LADDER[rung]));

        bool any = false;

        for (size_t i = 0; i < rs->n; i++) {
            if (rs->v[i].match != LADDER[rung] || rs->v[i].op != in->op) {
                continue;
            }
            any = true;
            ncc_buffer_printf(b, "%s%s", rule_matches(m, rs, i, in, key, ir_type_id(m, key))
                                      ? " MATCH "
                                      : " miss ",
                      rs->v[i].name);
        }

        if (!any) {
            ncc_buffer_puts(b, " no rule at this rung");
        }
        ncc_buffer_puts(b, "\n");
    }

    ir_match_t       how = IR_MATCH_DEFAULT;
    const ir_rule_t *r   = ir_rule_resolve(m, rs, in, &how);

    if (r) {
        ncc_buffer_printf(b, "resolves to %s by %s\n", r->name, ir_match_name(how));
    }
    else {
        ncc_buffer_puts(b, "resolves to nothing\n");
    }

    return ncc_buffer_take(b);
}
