#ifndef IR_DIAG_H
#define IR_DIAG_H

#include "ir/ir.h"
#include "lib/string.h"
#include "diag/domain.h"

#define IR_DIAG(id, code, sev, text) IR_DIAG_##id,

typedef enum : uint16_t {
#include "ir_diags.def"
    IR_DIAG_COUNT,
} ir_diagcode_t;

#undef IR_DIAG

core_string_t        ir_diag_code(ir_diagcode_t c);
core_string_t        ir_diag_default_text(ir_diagcode_t c);
core_diag_severity_t ir_diag_default_severity(ir_diagcode_t c);
bool                 ir_diag_code_lookup(core_string_t code, ir_diagcode_t *out);
extern const core_diag_domain_t ir_diag_domain;

typedef enum : uint16_t {
#define IR_TEXT(id, text) IR_TEXT_##id,
#include "ir_texts.def"
#undef IR_TEXT
    IR_TEXT_COUNT,
} ir_text_t;

typedef core_text_message_t ir_message_t;

#define IR_MESSAGE(id) ((ir_message_t){.text = IR_TEXT_##id})
#define IR_MESSAGE_ARGS(id, ...) CORE_TEXT_MESSAGE(IR_TEXT_##id, __VA_ARGS__)
#define IR_DETAIL(value) IR_MESSAGE_ARGS(DETAIL, CORE_TEXT_STRING(value))
#define IR_NO_LABEL ((ir_message_t){.text = IR_TEXT_COUNT})

typedef struct {
    ir_diagcode_t code;
    ir_span_t     span;
    size_t        index;
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
bool         ir_policy_set(ir_policy_t *p, core_string_t code_or_prefix,
                           ir_outcome_t outcome);
ir_outcome_t ir_policy_for(const ir_policy_t *p, core_string_t code);

typedef struct ir_diags_t ir_diags_t;

ir_diags_t *ir_diags_new(ir_ctx_t *ctx, ir_policy_t *policy);
ir_diags_t *ir_diags_new_attached(
    ir_ctx_t *ctx, ir_policy_t *policy, core_diagnostics_t *destination,
    const core_translation_unit_t *unit);

typedef size_t (*ir_diag_report_fn)(void *state, ir_diags_t *d,
                                    ir_diagcode_t code,
                                    core_diag_severity_t severity,
                                    core_diag_location_t at,
                                    core_string_t message);

void ir_diags_set_reporter(ir_diags_t *d, ir_diag_report_fn report,
                           void *state);

typedef core_string_t (*ir_text_fn)(void *state, ir_message_t message);

void ir_diags_set_text(ir_diags_t *d, ir_text_fn text, void *state);
core_string_t ir_message_default_text(ir_message_t message);
core_diagnostics_t *ir_diags_destination(const ir_diags_t *d);
ir_span_t ir_diag_name_span(ir_span_t span, core_string_t name);
const core_translation_unit_t *ir_diags_unit(const ir_diags_t *d);

ir_diag_t *ir_diag_begin(ir_diags_t *d, ir_diagcode_t code, ir_span_t span,
                         ir_message_t label);
ir_diag_t *ir_diag_begin_at(ir_diags_t *d, ir_diagcode_t code,
                            const ir_instr_t *in, const ir_block_t *b,
                            const ir_func_t *f, ir_message_t label);
bool       ir_diag_message(ir_diag_t *dg, ir_diags_t *d,
                           ir_message_t message);
bool       ir_diag_secondary(ir_diags_t *d, ir_diag_t *dg, ir_span_t span,
                             ir_message_t label);
bool       ir_diag_help(ir_diags_t *d, ir_diag_t *dg, ir_message_t text);

uint32_t                 ir_diags_count(const ir_diags_t *d);
const ir_diag_t         *ir_diags_at(const ir_diags_t *d, uint32_t i);
const core_diagnostic_t *ir_diag_item(const ir_diags_t *d,
                                      const ir_diag_t *dg);
bool                     ir_diags_failed(const ir_diags_t *d);
uint32_t                 ir_import_type_errors(ir_module_t *m, ir_diags_t *d);

typedef core_string_t (*ir_source_fn)(void *state, int32_t file);

typedef struct {
    ir_source_fn       source;
    void              *state;
    const ir_module_t *module;
    core_string_t      path;
    bool               color;
} ir_render_t;

core_string_t ir_diags_render_text(ir_diags_t *d, const ir_render_t *r);
core_string_t ir_diags_render_json(ir_diags_t *d, const ir_render_t *r);

#endif
