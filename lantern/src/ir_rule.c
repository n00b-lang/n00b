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

ir_rules_t *ir_rules_new(ir_ctx_t *ctx)
{
    if (!ctx) {
        return nullptr;
    }

    ir_rules_t *rs = ir_alloc(ctx, sizeof(ir_rules_t));

    if (rs) {
        rs->ctx = ctx;
    }

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
    if (!rs || rule.op >= IR_OP_COUNT || !rule.name) {
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

static void ready_keys(ir_module_t *m, ir_rules_t *rs)
{
    if (rs->keys_ready) {
        return;
    }

    rs->keys = ir_alloc(rs->ctx, (rs->n ? rs->n : 1) * sizeof(uint64_t));

    for (size_t i = 0; i < rs->n; i++) {
        rs->keys[i] = 0;

        if (rs->v[i].match != IR_MATCH_EXACT || !rs->v[i].type_text) {
            continue;
        }

        th_type *t = ir_type_parse(m, rs->v[i].type_text);

        rs->keys[i] = t ? ir_type_id(m, t) : 0;
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
                         ir_instr_t *in, th_type *key)
{
    const ir_rule_t *r = &rs->v[i];

    if (r->op != in->op) {
        return false;
    }

    switch (r->match) {
    case IR_MATCH_EXACT:
        return key && rs->keys[i] && ir_type_id(m, key) == rs->keys[i];

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

static const ir_match_t LADDER[] = {
    IR_MATCH_EXACT, IR_MATCH_CONSTRUCTOR, IR_MATCH_KIND, IR_MATCH_CLASS,
    IR_MATCH_DEFAULT,
};

const ir_rule_t *ir_rule_resolve(ir_module_t *m, ir_rules_t *rs,
                                 ir_instr_t *in, ir_match_t *how)
{
    if (!m || !rs || !in) {
        return nullptr;
    }
    ready_keys(m, rs);

    th_type *key = ir_instr_key_type(in);

    for (size_t rung = 0; rung < sizeof(LADDER) / sizeof(LADDER[0]); rung++) {
        for (size_t i = 0; i < rs->n; i++) {
            if (rs->v[i].match != LADDER[rung]) {
                continue;
            }

            if (rule_matches(m, rs, i, in, key)) {
                if (how) {
                    *how = LADDER[rung];
                }

                return &rs->v[i];
            }
        }
    }

    return nullptr;
}

ir_rule_result_t ir_rule_apply(ir_module_t *m, ir_rules_t *rs, ir_instr_t *in,
                               ir_diags_t *d)
{
    ir_rule_result_t out = {.verdict = IR_RULE_DECLINE};

    if (!m || !rs || !in) {
        return out;
    }
    ready_keys(m, rs);

    th_type *key = ir_instr_key_type(in);

    for (size_t rung = 0; rung < sizeof(LADDER) / sizeof(LADDER[0]); rung++) {
        for (size_t i = 0; i < rs->n; i++) {
            if (rs->v[i].match != LADDER[rung] || !rs->v[i].type_rule) {
                continue;
            }

            if (!rule_matches(m, rs, i, in, key)) {
                continue;
            }

            ir_rule_result_t r = rs->v[i].type_rule(m, in, d, rs->v[i].state);

            if (r.verdict == IR_RULE_DECLINE) {
                continue;
            }

            if (r.verdict == IR_RULE_ACCEPT) {
                ir_annot_set(m->ctx, &in->annots,
                             (ir_annot_t){.key          = "ir.rule",
                                          .kind         = IR_ANNOT_STR,
                                          .s            = rs->v[i].name,
                                          .derived_from = IR_PROP_TYPED});

                for (uint32_t k = 0; k < r.n_annots; k++) {
                    ir_annot_set(m->ctx, &in->annots, r.annots[k]);
                }
            }

            return r;
        }
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

    th_type *key = ir_instr_key_type(in);

    for (size_t rung = 0; rung < sizeof(LADDER) / sizeof(LADDER[0]); rung++) {
        for (size_t i = 0; i < rs->n; i++) {
            if (rs->v[i].match != LADDER[rung] || !rs->v[i].lower_rule) {
                continue;
            }

            if (!rule_matches(m, rs, i, in, key)) {
                continue;
            }

            if (rs->v[i].lower_rule(m, in, d, rs->v[i].state)) {
                return true;
            }
        }
    }

    return false;
}

char *ir_rule_explain(ir_module_t *m, ir_rules_t *rs, ir_instr_t *in)
{
    ir_sb b = {0};

    if (!m || !rs || !in) {
        ir_sb_str(&b, "no instruction\n");

        return ir_sb_take(&b);
    }

    th_type *key = ir_instr_key_type(in);

    ir_sb_fmt(&b, "opcode %s\n", ir_op_spelling(in->op));
    ir_sb_str(&b, "key type ");

    if (key) {
        ir_sb_str(&b, ir_type_text(m, key));
    }
    else {
        ir_sb_str(&b, "none");
    }
    ir_sb_str(&b, "\n");

    ready_keys(m, rs);

    for (size_t rung = 0; rung < sizeof(LADDER) / sizeof(LADDER[0]); rung++) {
        ir_sb_fmt(&b, "  %-16s", ir_match_name(LADDER[rung]));

        bool any = false;

        for (size_t i = 0; i < rs->n; i++) {
            if (rs->v[i].match != LADDER[rung] || rs->v[i].op != in->op) {
                continue;
            }
            any = true;
            ir_sb_fmt(&b, "%s%s", rule_matches(m, rs, i, in, key)
                                      ? " MATCH "
                                      : " miss ",
                      rs->v[i].name);
        }

        if (!any) {
            ir_sb_str(&b, " no rule at this rung");
        }
        ir_sb_str(&b, "\n");
    }

    ir_match_t       how = IR_MATCH_DEFAULT;
    const ir_rule_t *r   = ir_rule_resolve(m, rs, in, &how);

    if (r) {
        ir_sb_fmt(&b, "resolves to %s by %s\n", r->name, ir_match_name(how));
    }
    else {
        ir_sb_str(&b, "resolves to nothing\n");
    }

    return ir_sb_take(&b);
}
