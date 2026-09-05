#ifndef IR_DIAG_H
#define IR_DIAG_H

#include "ir/ir.h"

typedef enum : uint8_t {
    IR_SEV_FATAL,
    IR_SEV_ERROR,
    IR_SEV_WARNING,
    IR_SEV_NOTE,
    IR_SEV_HELP,
} ir_severity_t;

const char *ir_severity_name(ir_severity_t s);

#define IR_DIAG(id, code, sev, text) IR_DIAG_##id,

typedef enum : uint16_t {
#include "ir_diags.def"
    IR_DIAG_COUNT,
} ir_diagcode_t;

#undef IR_DIAG

const char   *ir_diag_code(ir_diagcode_t c);
const char   *ir_diag_default_text(ir_diagcode_t c);
ir_severity_t ir_diag_default_severity(ir_diagcode_t c);
bool          ir_diag_code_lookup(const char *code, ir_diagcode_t *out);

typedef struct {
    ir_span_t   span;
    const char *label;
} ir_label_t;

typedef struct {
    ir_span_t   span;
    const char *replacement;
    const char *description;
} ir_suggestion_t;

typedef struct {
    ir_severity_t    severity;
    ir_diagcode_t    code;
    const char      *message;
    ir_label_t       primary;
    ir_label_t      *secondary;
    uint32_t         n_secondary;
    ir_suggestion_t *suggestions;
    uint32_t         n_suggestions;
    ir_origin_t     *origin;
    uint64_t         seq;
} ir_diag_t;

typedef enum : uint8_t {
    IR_POLICY_DEFAULT,
    IR_POLICY_ALLOW,
    IR_POLICY_WARN,
    IR_POLICY_DENY,
    IR_POLICY_FORBID,
} ir_outcome_t;

typedef struct ir_policy_t ir_policy_t;

ir_policy_t *ir_policy_new(ir_ctx_t *ctx);
bool         ir_policy_set(ir_policy_t *p, const char *code_or_prefix,
                           ir_outcome_t outcome);
ir_outcome_t ir_policy_for(const ir_policy_t *p, const char *code);

typedef struct ir_diags_t ir_diags_t;

ir_diags_t *ir_diags_new(ir_ctx_t *ctx, ir_policy_t *policy);

ir_diag_t *ir_diag_begin(ir_diags_t *d, ir_diagcode_t code, ir_span_t span,
                         const char *label);
ir_diag_t *ir_diag_begin_at(ir_diags_t *d, ir_diagcode_t code,
                            const ir_instr_t *in, const ir_block_t *b,
                            const ir_func_t *f, const char *label);
bool       ir_diag_message(ir_diag_t *dg, ir_diags_t *d, const char *message);
bool       ir_diag_secondary(ir_diags_t *d, ir_diag_t *dg, ir_span_t span,
                             const char *label);
bool       ir_diag_suggest(ir_diags_t *d, ir_diag_t *dg, ir_span_t span,
                           const char *replacement, const char *description);
bool       ir_diag_origin(ir_diag_t *dg, ir_origin_t *o);
bool       ir_diag_note(ir_diags_t *d, ir_severity_t sev, ir_diagcode_t code,
                        ir_span_t span, const char *label);

uint32_t         ir_diags_count(const ir_diags_t *d);
const ir_diag_t *ir_diags_at(ir_diags_t *d, uint32_t i);
bool             ir_diags_failed(const ir_diags_t *d);
bool             ir_diags_fatal(const ir_diags_t *d);
uint32_t         ir_diags_count_of(const ir_diags_t *d, ir_severity_t sev);
bool             ir_diags_merge(ir_diags_t *dst, ir_diags_t *src);
void             ir_diags_sort(ir_diags_t *d);

typedef const char *(*ir_source_fn)(void *state, int32_t file, size_t *len);

typedef struct {
    ir_source_fn       source;
    void              *state;
    const ir_module_t *module;
    bool               color;
} ir_render_t;

char *ir_diags_render_text(ir_diags_t *d, const ir_render_t *r);
char *ir_diags_render_json(ir_diags_t *d, const ir_render_t *r);

void     ir_diags_mark_seen(const ir_diags_t *d);
bool     ir_diag_code_seen(ir_diagcode_t c);
uint32_t ir_diag_unseen(const ir_diagcode_t **out);

#endif
