#ifndef IR_RULE_H
#define IR_RULE_H

#include "ir/diag.h"
#include "ir/ir.h"

typedef enum : uint8_t {
    IR_RULE_ACCEPT,
    IR_RULE_DECLINE,
    IR_RULE_REJECT,
} ir_verdict_t;

typedef struct {
    ir_verdict_t verdict;
    th_type     *result;
    ir_annot_t  *annots;
    uint32_t     n_annots;
} ir_rule_result_t;

typedef enum : uint8_t {
    IR_MATCH_EXACT,
    IR_MATCH_CONSTRUCTOR,
    IR_MATCH_KIND,
    IR_MATCH_CLASS,
    IR_MATCH_DEFAULT,
} ir_match_t;

const char *ir_match_name(ir_match_t m);

typedef struct ir_rules_t ir_rules_t;

typedef ir_rule_result_t (*ir_type_rule_fn)(ir_module_t *m, ir_instr_t *in,
                                            ir_diags_t *d, void *state);

typedef bool (*ir_lower_rule_fn)(ir_module_t *m, ir_instr_t *in,
                                 ir_diags_t *d, void *state);

typedef struct ir_rule_t ir_rule_t;

struct ir_rule_t {
    const char      *name;
    ir_op_t          op;
    ir_match_t       match;
    const char      *type_text;
    th_kind          kind;
    ir_class_t       class;
    ir_type_rule_fn  type_rule;
    ir_lower_rule_fn lower_rule;
    void            *state;
};

ir_rules_t *ir_rules_new(ir_ctx_t *ctx);
ir_rules_t *ir_rules_from_profile(ir_ctx_t *ctx, const ir_profile_t *p);
bool        ir_rules_add(ir_rules_t *rs, ir_rule_t rule);
size_t      ir_rules_count(const ir_rules_t *rs);

const ir_rule_t *ir_rule_resolve(ir_module_t *m, ir_rules_t *rs,
                                 ir_instr_t *in, ir_match_t *how);

ir_rule_result_t ir_rule_apply(ir_module_t *m, ir_rules_t *rs, ir_instr_t *in,
                               ir_diags_t *d);
bool             ir_rule_lower(ir_module_t *m, ir_rules_t *rs, ir_instr_t *in,
                               ir_diags_t *d);

char *ir_rule_explain(ir_module_t *m, ir_rules_t *rs, ir_instr_t *in);

th_type *ir_instr_key_type(ir_instr_t *in);

#endif
