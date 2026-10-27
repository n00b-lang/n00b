#include <stdlib.h>

#include "private/ir/internal.h"
#include "ir/diag.h"
#include "lib/buffer.h"

static const core_diag_code_t codes[IR_DIAG_COUNT] = {
#define IR_DIAG(id, code, sev, text) \
    {CORE_STRING_STATIC(code), CORE_DIAG_##sev, CORE_STRING_STATIC(text), {}},
#include "ir/ir_diags.def"
#undef IR_DIAG
};

static const core_text_form_t text_forms[] = {
#define IR_TEXT(id, text) {CORE_STRING_STATIC(text), CORE_STRING_STATIC(text)},
#include "ir/ir_texts.def"
#undef IR_TEXT
};

static const core_text_catalog_t text_catalog = {
    .forms = text_forms,
    .count = IR_TEXT_COUNT,
};

const core_diag_domain_t ir_diag_domain = {
    .codes = codes,
    .count = IR_DIAG_COUNT,
    .texts = &text_catalog,
};

core_string_t ir_diag_code(ir_diagcode_t c)
{
    return c < IR_DIAG_COUNT ? codes[c].code : (core_string_t){};
}

core_string_t ir_diag_default_text(ir_diagcode_t c)
{
    return c < IR_DIAG_COUNT ? codes[c].title : (core_string_t){};
}

core_diag_severity_t ir_diag_default_severity(ir_diagcode_t c)
{
    return c < IR_DIAG_COUNT ? codes[c].severity : CORE_DIAG_ERROR;
}

bool ir_diag_code_lookup(core_string_t code, ir_diagcode_t *out)
{
    uint32_t id = 0;
    if (!core_diag_domain_lookup(&ir_diag_domain, code, &id)) return false;
    if (out) *out = (ir_diagcode_t)id;
    return true;
}

typedef struct {
    core_string_t key;
    ir_outcome_t  outcome;
} ir_rule_entry_t;

struct ir_policy_t {
    ir_ctx_t        *ctx;
    ir_rule_entry_t *rules;
    size_t           n;
    size_t           cap;
    void            *index;
};

ir_policy_t *ir_policy_new(ir_ctx_t *ctx)
{
    if (!ctx) {
        return nullptr;
    }

    ir_policy_t *p = ir_alloc(ctx, sizeof(ir_policy_t));

    p->ctx = ctx;

    return p;
}

bool ir_policy_set(ir_policy_t *p, core_string_t key, ir_outcome_t outcome)
{
    if (!p || !key.data || !key.u8_bytes) {
        return false;
    }

    uintptr_t at = (uintptr_t)ir_index_find(p->index, key);

    if (at) {
        p->rules[at - 1].outcome = outcome;

        return true;
    }

    p->rules = ir_grow(p->ctx, p->rules, &p->cap, p->n + 1,
                       sizeof(ir_rule_entry_t));

    if (!p->rules) {
        return false;
    }

    p->rules[p->n].key     = ir_strdup(p->ctx, key);
    p->rules[p->n].outcome = outcome;
    p->n++;

    ir_index_add(p->ctx, &p->index, p->rules[p->n - 1].key,
                 (void *)(uintptr_t)p->n);

    return true;
}

static const ir_rule_entry_t *
policy_rule(const ir_policy_t *p, core_string_t code)
{
    const ir_rule_entry_t *best = nullptr;

    for (size_t i = 0; p && code.data && i < p->n; i++) {
        if (core_string_starts_with(code, p->rules[i].key)
            && (!best || p->rules[i].key.u8_bytes > best->key.u8_bytes)) {
            best = &p->rules[i];
        }
    }

    return best;
}

ir_outcome_t ir_policy_for(const ir_policy_t *p, core_string_t code)
{
    const ir_rule_entry_t *rule = policy_rule(p, code);

    return rule ? rule->outcome : IR_POLICY_DEFAULT;
}

struct ir_diags_t {
    ir_ctx_t                      *ctx;
    ir_policy_t                   *policy;
    core_diagnostics_t            *items;
    const core_translation_unit_t *unit;
    bool                           attached;
    ir_diag_report_fn              report;
    void                          *report_state;
    ir_text_fn                     text;
    void                          *text_state;
    ir_diag_t                     *v;
    uint32_t                       n;
    uint32_t                       cap;
};

static core_diag_location_t
location_of(ir_span_t span)
{
    if (!ir_span_known(span)) return (core_diag_location_t){0};
    return (core_diag_location_t){
        .line = span.start_line,
        .column = span.start_col,
        .end_line = span.end_line,
        .end_column = span.end_col,
        .has_location = true,
    };
}

static void free_items(void *items)
{
    core_diagnostics_free(items);
}

ir_diags_t *ir_diags_new(ir_ctx_t *ctx, ir_policy_t *policy)
{
    if (!ctx) {
        return nullptr;
    }

    ir_diags_t *d = ir_alloc(ctx, sizeof(ir_diags_t));

    d->ctx    = ctx;
    d->policy = policy;
    d->items  = core_diagnostics_new();
    ir_ctx_on_free(ctx, free_items, d->items);

    return d;
}

ir_diags_t *ir_diags_new_attached(
    ir_ctx_t *ctx, ir_policy_t *policy, core_diagnostics_t *destination,
    const core_translation_unit_t *unit)
{
    if (!ctx || !destination || !unit) return nullptr;
    ir_diags_t *d = ir_alloc(ctx, sizeof(ir_diags_t));
    d->ctx      = ctx;
    d->policy   = policy;
    d->items    = destination;
    d->unit     = unit;
    d->attached = true;
    return d;
}

void ir_diags_set_reporter(ir_diags_t *d, ir_diag_report_fn report,
                           void *state)
{
    if (!d) return;
    d->report = report;
    d->report_state = state;
}

core_string_t ir_message_default_text(ir_message_t message)
{
    return core_text_render(&text_catalog, message.text, message.args);
}

void ir_diags_set_text(ir_diags_t *d, ir_text_fn text, void *state)
{
    if (!d) return;
    d->text = text;
    d->text_state = state;
}

static core_string_t render(const ir_diags_t *d, ir_message_t message)
{
    return d->text ? d->text(d->text_state, message)
                   : ir_message_default_text(message);
}

ir_span_t ir_diag_name_span(ir_span_t span, core_string_t name)
{
    uint32_t columns = 0;

    for (size_t i = 0; i < name.u8_bytes; i++) {
        columns += ((uint8_t)name.data[i] & 0xc0) != 0x80;
    }
    if (!ir_span_known(span) || !name.u8_bytes
        || span.start_line != span.end_line
        || span.end_byte < span.start_byte + name.u8_bytes
        || span.end_col < span.start_col + columns) {
        return IR_SPAN_UNKNOWN;
    }
    span.end_byte = span.start_byte + (uint32_t)name.u8_bytes;
    span.end_col  = span.start_col + columns;

    return span;
}

core_diagnostics_t *ir_diags_destination(const ir_diags_t *d)
{
    return d && d->attached ? d->items : nullptr;
}

const core_translation_unit_t *ir_diags_unit(const ir_diags_t *d)
{
    return d ? d->unit : nullptr;
}

static bool
apply_policy(const ir_policy_t *policy, core_diag_severity_t *severity,
             core_string_t code)
{
    switch (ir_policy_for(policy, code)) {
    case IR_POLICY_ALLOW: return false;
    case IR_POLICY_WARN: *severity = CORE_DIAG_WARNING; break;
    case IR_POLICY_DENY:
    case IR_POLICY_FORBID: *severity = CORE_DIAG_ERROR; break;
    default: break;
    }
    return true;
}

static void
note_promotion(ir_diags_t *d, size_t index, core_string_t code)
{
    const ir_rule_entry_t *rule = policy_rule(d->policy, code);
    core_string_t text = render(d, rule
        ? IR_MESSAGE_ARGS(PROMOTED_BY_RULE, CORE_TEXT_STRING(rule->key))
        : IR_MESSAGE(PROMOTED));
    core_diagnostic_add_part(d->items, index, CORE_DIAG_PART_NOTE,
                             (core_string_t){}, (core_diag_location_t){},
                             text);
    core_free(text.data);
}

static size_t
report(ir_diags_t *d, ir_diagcode_t code, core_diag_severity_t severity,
       ir_span_t span)
{
    core_diag_location_t at = location_of(span);
    core_string_t message = ir_diag_default_text(code);
    if (d->report)
        return d->report(d->report_state, d, code, severity, at, message);
    size_t index = core_diagnostics_count(d->items);
    if (!core_diagnostics_report_at(d->items, d->unit, severity,
            ir_diag_code(code), message, at)
        || core_diagnostics_count(d->items) == index)
        return SIZE_MAX;
    return index;
}

static ir_diag_t *push(ir_diags_t *d, ir_diagcode_t code, ir_span_t span,
                       ir_message_t label)
{
    if (!d || code >= IR_DIAG_COUNT) {
        return nullptr;
    }

    core_diag_severity_t declared = ir_diag_default_severity(code);
    core_diag_severity_t severity = declared;
    if (!apply_policy(d->policy, &severity, ir_diag_code(code))) {
        return nullptr;
    }
    size_t index = report(d, code, severity, span);
    if (index == SIZE_MAX) {
        return nullptr;
    }
    if (label.text < IR_TEXT_COUNT) {
        core_string_t text = render(d, label);
        core_diagnostic_set_label(d->items, index, text);
        core_free(text.data);
    }
    if (!d->report && declared == CORE_DIAG_WARNING
        && severity == CORE_DIAG_ERROR) {
        note_promotion(d, index, ir_diag_code(code));
    }

    size_t cap = d->cap;

    d->v = ir_grow(d->ctx, d->v, &cap, d->n + 1u, sizeof(ir_diag_t));
    if (!d->v) {
        return nullptr;
    }
    d->cap = (uint32_t)cap;
    d->v[d->n] = (ir_diag_t){.code = code, .span = span, .index = index};

    return &d->v[d->n++];
}

ir_diag_t *ir_diag_begin(ir_diags_t *d, ir_diagcode_t code, ir_span_t span,
                         ir_message_t label)
{
    return push(d, code, span, label);
}

ir_diag_t *ir_diag_begin_at(ir_diags_t *d, ir_diagcode_t code,
                            const ir_instr_t *in, const ir_block_t *b,
                            const ir_func_t *f, ir_message_t label)
{
    ir_span_t span = IR_SPAN_UNKNOWN;

    if (in && ir_span_known(in->span)) {
        span = in->span;
    }
    else if (in && in->block && ir_span_known(in->block->span)) {
        span = in->block->span;
    }
    else if (b && ir_span_known(b->span)) {
        span = b->span;
    }
    else if (in && in->block && in->block->owner
             && ir_span_known(in->block->owner->span)) {
        span = in->block->owner->span;
    }
    else if (b && b->owner && ir_span_known(b->owner->span)) {
        span = b->owner->span;
    }
    else if (f && ir_span_known(f->span)) {
        span = f->span;
    }

    if (!ir_span_known(span)) {
        const ir_func_t *owner = f                       ? f
                              : b                        ? b->owner
                              : in && in->block          ? in->block->owner
                                                         : nullptr;

        if (owner && owner->owner) {
            span = ir_module_span(owner->owner);
        }
    }

    return push(d, code, span, label);
}

bool ir_diag_message(ir_diag_t *dg, ir_diags_t *d, ir_message_t message)
{
    if (!dg || !d) return false;
    core_string_t text = render(d, message);
    bool ok = text.data
        && core_diagnostic_set_message(d->items, dg->index, text);
    core_free(text.data);
    return ok;
}

bool ir_diag_secondary(ir_diags_t *d, ir_diag_t *dg, ir_span_t span,
                       ir_message_t label)
{
    if (!d || !dg) {
        return false;
    }
    core_string_t text = render(d, label);
    bool ok = core_diagnostic_add_part(d->items, dg->index,
        ir_span_known(span) ? CORE_DIAG_PART_SPAN : CORE_DIAG_PART_NOTE,
        (core_string_t){}, location_of(span), text);
    core_free(text.data);
    return ok;
}

bool ir_diag_help(ir_diags_t *d, ir_diag_t *dg, ir_message_t message)
{
    if (!d || !dg) return false;
    core_string_t text = render(d, message);
    bool ok = text.u8_bytes
        && core_diagnostic_add_part(d->items, dg->index, CORE_DIAG_PART_HELP,
                                    (core_string_t){},
                                    (core_diag_location_t){}, text);
    core_free(text.data);
    return ok;
}

uint32_t ir_diags_count(const ir_diags_t *d)
{
    return d ? d->n : 0;
}

const ir_diag_t *ir_diags_at(const ir_diags_t *d, uint32_t i)
{
    return d && i < d->n ? &d->v[i] : nullptr;
}

const core_diagnostic_t *ir_diag_item(const ir_diags_t *d,
                                      const ir_diag_t *dg)
{
    return d && dg ? core_diagnostics_at(d->items, dg->index) : nullptr;
}

bool ir_diags_failed(const ir_diags_t *d)
{
    return d && core_diagnostics_failed(d->items);
}

static core_string_t file_name(const ir_render_t *r, int32_t file)
{
    core_string_t p = r && r->module && file >= 0
        ? ir_file_path(r->module, file) : (core_string_t){};

    if (!p.data && r && file == 0 && r->path.u8_bytes) {
        p = r->path;
    }

    return p.data ? p : CORE_STRING_STATIC("<unknown>");
}

static core_string_t source_for_path(void *context, core_string_t path)
{
    const ir_render_t *r = context;

    if (!r || !r->source) {
        return (core_string_t){};
    }

    size_t count = r->module ? ir_file_count(r->module) : 0;

    for (size_t file = 0; file < (count ? count : 1); file++) {
        if (core_string_eq(file_name(r, (int32_t)file), path)) {
            return r->source(r->state, (int32_t)file);
        }
    }

    return (core_string_t){};
}

static void name_files(ir_diags_t *d, const ir_render_t *r)
{
    for (uint32_t i = 0; i < d->n; i++) {
        if (ir_span_known(d->v[i].span)) {
            core_diagnostic_set_path(d->items, d->v[i].index,
                                     file_name(r, d->v[i].span.file));
        }
    }
}

core_string_t ir_diags_render_text(ir_diags_t *d, const ir_render_t *r)
{
    if (!d) {
        return (core_string_t){};
    }
    name_files(d, r);
    return core_diagnostics_render(d->items, (core_diag_render_t){
        .color = r && r->color,
        .sorted = true,
        .source = source_for_path,
        .source_context = (void *)r,
    });
}

core_string_t ir_diags_render_json(ir_diags_t *d, const ir_render_t *r)
{
    if (!d) {
        return (core_string_t){};
    }
    name_files(d, r);
    return core_diagnostics_render_json(d->items);
}
