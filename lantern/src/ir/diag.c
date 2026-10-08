#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/diag.h"
#include "lib/dict.h"

#define IR_DIAG(id, code, sev, text) {code, IR_SEV_##sev, text},

static const struct {
    const char   *code;
    ir_severity_t severity;
    const char   *text;
} codes[IR_DIAG_COUNT] = {
#include "ir/ir_diags.def"
};

#undef IR_DIAG

const char *ir_severity_name(ir_severity_t s)
{
    switch (s) {
    case IR_SEV_ERROR:
        return "error";
    case IR_SEV_WARNING:
        return "warning";
    case IR_SEV_NOTE:
        return "note";
    case IR_SEV_HELP:
        return "help";
    default:
        return "?";
    }
}

const char *ir_diag_code(ir_diagcode_t c)
{
    return c < IR_DIAG_COUNT ? codes[c].code : nullptr;
}

const char *ir_diag_default_text(ir_diagcode_t c)
{
    return c < IR_DIAG_COUNT ? codes[c].text : nullptr;
}

ir_severity_t ir_diag_default_severity(ir_diagcode_t c)
{
    return c < IR_DIAG_COUNT ? codes[c].severity : IR_SEV_ERROR;
}

static core_dict_t code_index;
static bool        code_ready;

static void free_code_index(void)
{
    core_dict_free(&code_index);
    code_ready = false;
}

static void build_code_index(void)
{
    core_dict_init(&code_index, core_hash_cstring, core_dict_cstr_eq);

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        _core_dict_put(&code_index, (void *)codes[i].code,
                       (void *)(uintptr_t)i);
    }

    code_ready = true;
    atexit(free_code_index);
}

bool ir_diag_code_lookup(const char *code, ir_diagcode_t *out)
{
    if (!code) {
        return false;
    }

    if (!code_ready) {
        build_code_index();
    }

    bool  found = false;
    void *val   = _core_dict_get(&code_index, (void *)code, &found);

    if (found && out) {
        *out = (ir_diagcode_t)(uintptr_t)val;
    }

    return found;
}

typedef struct {
    const char  *key;
    ir_outcome_t outcome;
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

bool ir_policy_set(ir_policy_t *p, const char *key, ir_outcome_t outcome)
{
    if (!p || !key || !*key) {
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

ir_outcome_t ir_policy_for(const ir_policy_t *p, const char *code)
{
    if (!p || !code) {
        return IR_POLICY_DEFAULT;
    }

    ir_outcome_t best     = IR_POLICY_DEFAULT;
    size_t       best_len = 0;
    bool         found    = false;

    for (size_t i = 0; i < p->n; i++) {
        const char *k = p->rules[i].key;
        size_t      n = strlen(k);

        if (strncmp(code, k, n)) {
            continue;
        }

        if (!found || n > best_len) {
            best     = p->rules[i].outcome;
            best_len = n;
            found    = true;
        }
    }

    return best;
}

struct ir_diags_t {
    ir_ctx_t    *ctx;
    ir_policy_t *policy;
    core_diagnostics_t *core;
    core_diagnostics_t *destination;
    const core_translation_unit_t *unit;
    ir_diag_t   *v;
    uint32_t     n;
    uint32_t     cap;
    uint64_t     seq;
    bool         sorted;
    bool         anchored;
};

static bool core_policy(void *, core_diag_severity_t *, core_string_t);

ir_diags_t *ir_diags_new(ir_ctx_t *ctx, ir_policy_t *policy)
{
    if (!ctx) {
        return nullptr;
    }

    ir_diags_t *d = ir_alloc(ctx, sizeof(ir_diags_t));

    d->ctx    = ctx;
    d->policy = policy;
    d->core   = core_diagnostics_new();
    d->sorted = true;
    core_diagnostics_set_policy(d->core, core_policy, policy);
    ir_ctx_on_free(ctx, (void (*)(void *))core_diagnostics_free, d->core);

    return d;
}

ir_diags_t *ir_diags_new_attached(
    ir_ctx_t *ctx, ir_policy_t *policy, core_diagnostics_t *destination,
    const core_translation_unit_t *unit)
{
    if (!destination || !unit) return nullptr;
    ir_diags_t *d = ir_diags_new(ctx, policy);
    if (!d) return nullptr;
    d->destination = destination;
    d->unit = unit;
    return d;
}

static bool is_attachment(ir_severity_t s)
{
    return s == IR_SEV_NOTE || s == IR_SEV_HELP;
}

static core_diag_severity_t
core_severity_of(ir_severity_t severity)
{
    return severity == IR_SEV_WARNING ? CORE_DIAG_WARNING
        : severity == IR_SEV_NOTE ? CORE_DIAG_NOTE
        : severity == IR_SEV_HELP ? CORE_DIAG_HELP : CORE_DIAG_ERROR;
}

static bool
core_policy(void *context, core_diag_severity_t *severity, core_string_t code)
{
    ir_policy_t *policy = context;
    ir_outcome_t outcome = ir_policy_for(policy, code.data);
    bool attached = *severity == CORE_DIAG_NOTE || *severity == CORE_DIAG_HELP;
    switch (outcome) {
    case IR_POLICY_ALLOW: return false;
    case IR_POLICY_WARN:
        if (!attached) *severity = CORE_DIAG_WARNING;
        break;
    case IR_POLICY_DENY:
    case IR_POLICY_FORBID:
        if (!attached) *severity = CORE_DIAG_ERROR;
        break;
    default: break;
    }
    return true;
}

static ir_diag_t *push(ir_diags_t *d, ir_diagcode_t code, ir_severity_t sev,
                       ir_span_t span, const char *label)
{
    if (!d || code >= IR_DIAG_COUNT) {
        return nullptr;
    }

    if (is_attachment(sev) && !d->anchored) {
        if (code != IR_DIAG_ORPHAN_NOTE) {
            ir_diag_t *dg = push(d, IR_DIAG_ORPHAN_NOTE, IR_SEV_ERROR, span,
                                 label);

            if (dg) {
                ir_diag_message(dg, d,
                                "a note or help has nothing to attach to, and "
                                "they never stand alone");
            }
        }

        return nullptr;
    }

    core_diag_severity_t core_severity = sev == IR_SEV_HELP ? CORE_DIAG_HELP
        : sev == IR_SEV_NOTE ? CORE_DIAG_NOTE
        : sev == IR_SEV_WARNING ? CORE_DIAG_WARNING : CORE_DIAG_ERROR;
    size_t core_index = core_diagnostics_count(d->core);
    if (!core_diagnostics_report(d->core, nullptr, core_severity,
            core_string_from_cstr(ir_diag_code(code)),
            core_string_from_cstr(ir_diag_default_text(code)))) {
        return nullptr;
    }
    const core_diagnostic_t *core_item =
        core_diagnostics_at(d->core, core_index);
    if (!core_item) {
        return nullptr;
    }
    sev = core_item->severity == CORE_DIAG_WARNING ? IR_SEV_WARNING
        : core_item->severity == CORE_DIAG_NOTE ? IR_SEV_NOTE
        : core_item->severity == CORE_DIAG_HELP ? IR_SEV_HELP : IR_SEV_ERROR;
    size_t destination_index = SIZE_MAX;
    if (d->destination && d->unit) {
        core_diag_location_t location = {0};
        if (ir_span_known(span)) {
            location = (core_diag_location_t){
                .line = span.start_line,
                .column = span.start_col,
                .end_line = span.end_line,
                .end_column = span.end_col,
                .has_location = true,
            };
        }
        destination_index = core_diagnostics_count(d->destination);
        if (!core_diagnostics_report_at(d->destination, d->unit,
                core_severity_of(sev),
                core_string_from_cstr(ir_diag_code(code)),
                core_string_from_cstr(ir_diag_default_text(code)), location)
            || core_diagnostics_count(d->destination) == destination_index)
            destination_index = SIZE_MAX;
    }
    size_t cap = d->cap;

    d->v = ir_grow(d->ctx, d->v, &cap, d->n + 1u, sizeof(ir_diag_t));

    if (!d->v) {
        return nullptr;
    }
    d->cap = (uint32_t)cap;

    ir_diag_t *dg = &d->v[d->n++];

    memset(dg, 0, sizeof(*dg));
    dg->severity      = sev;
    dg->code          = code;
    dg->message       = ir_diag_default_text(code);
    dg->primary.span  = span;
    dg->primary.label = label ? ir_strdup(d->ctx, label) : nullptr;
    dg->seq           = d->seq++;
    dg->core_index     = core_index;
    dg->destination_index = destination_index;

    d->sorted   = false;
    d->anchored = d->anchored
               || (!is_attachment(sev) && code != IR_DIAG_ORPHAN_NOTE);

    return dg;
}

ir_diag_t *ir_diag_begin(ir_diags_t *d, ir_diagcode_t code, ir_span_t span,
                         const char *label)
{
    return push(d, code, ir_diag_default_severity(code), span, label);
}

ir_diag_t *ir_diag_begin_at(ir_diags_t *d, ir_diagcode_t code,
                            const ir_instr_t *in, const ir_block_t *b,
                            const ir_func_t *f, const char *label)
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

    return push(d, code, ir_diag_default_severity(code), span, label);
}

bool ir_diag_note(ir_diags_t *d, ir_severity_t sev, ir_diagcode_t code,
                  ir_span_t span, const char *label)
{
    if (!is_attachment(sev)) {
        return false;
    }

    return push(d, code, sev, span, label) != nullptr;
}

bool ir_diag_message(ir_diag_t *dg, ir_diags_t *d, const char *message)
{
    if (!dg || !d || !message) {
        return false;
    }

    dg->message = ir_strdup(d->ctx, message);

    core_diagnostic_set_message(d->core, dg->core_index,
                                core_string_from_cstr(message));
    if (d->destination && dg->destination_index != SIZE_MAX)
        core_diagnostic_set_message(d->destination,
                                    dg->destination_index,
                                    core_string_from_cstr(message));

    return dg->message != nullptr;
}

bool ir_diag_secondary(ir_diags_t *d, ir_diag_t *dg, ir_span_t span,
                       const char *label)
{
    if (!d || !dg) {
        return false;
    }

    size_t cap = dg->cap_secondary;

    dg->secondary = ir_grow(d->ctx, dg->secondary, &cap, dg->n_secondary + 1u,
                            sizeof(ir_label_t));

    if (!dg->secondary) {
        return false;
    }
    dg->cap_secondary = (uint32_t)cap;

    dg->secondary[dg->n_secondary].span  = span;
    dg->secondary[dg->n_secondary].label = label ? ir_strdup(d->ctx, label)
                                                 : nullptr;
    dg->n_secondary++;

    return true;
}

bool ir_diag_suggest(ir_diags_t *d, ir_diag_t *dg, ir_span_t span,
                     const char *replacement, const char *description)
{
    if (!d || !dg || !replacement) {
        return false;
    }

    size_t cap = dg->cap_suggestions;

    dg->suggestions = ir_grow(d->ctx, dg->suggestions, &cap,
                              dg->n_suggestions + 1u,
                              sizeof(ir_suggestion_t));

    if (!dg->suggestions) {
        return false;
    }
    dg->cap_suggestions = (uint32_t)cap;

    ir_suggestion_t *s = &dg->suggestions[dg->n_suggestions++];

    s->span        = span;
    s->replacement = ir_strdup(d->ctx, replacement);
    s->description = description ? ir_strdup(d->ctx, description) : nullptr;

    return true;
}

bool ir_diag_origin(ir_diag_t *dg, ir_origin_t *o)
{
    if (!dg) {
        return false;
    }
    dg->origin = o;

    return true;
}

uint32_t ir_diags_count(const ir_diags_t *d)
{
    return d ? (uint32_t)core_diagnostics_count(d->core) : 0;
}

const ir_diag_t *ir_diags_at(ir_diags_t *d, uint32_t i)
{
    if (!d || i >= d->n) {
        return nullptr;
    }
    ir_diags_sort(d);

    return &d->v[i];
}

bool ir_diags_failed(const ir_diags_t *d)
{
    if (!d) return false;
    return d->destination ? core_diagnostics_failed(d->destination)
                          : core_diagnostics_failed(d->core);
}

uint32_t ir_diags_count_of(const ir_diags_t *d, ir_severity_t sev)
{
    if (!d) {
        return 0;
    }

    core_diag_severity_t core_sev = sev == IR_SEV_WARNING ? CORE_DIAG_WARNING
        : sev == IR_SEV_NOTE ? CORE_DIAG_NOTE
        : sev == IR_SEV_HELP ? CORE_DIAG_HELP : CORE_DIAG_ERROR;
    return (uint32_t)core_diagnostics_count_severity(d->core, core_sev);
}

bool ir_diags_merge(ir_diags_t *dst, ir_diags_t *src)
{
    if (!dst || !src) {
        return false;
    }

    if (dst->ctx != src->ctx) {
        return false;
    }

    size_t core_base = core_diagnostics_count(dst->core);
    if (!core_diagnostics_merge(dst->core, src->core)) return false;

    size_t cap = dst->cap;

    dst->v = ir_grow(dst->ctx, dst->v, &cap, dst->n + src->n,
                     sizeof(ir_diag_t));

    if (!dst->v) {
        return false;
    }
    dst->cap = (uint32_t)cap;

    for (uint32_t i = 0; i < src->n; i++) {
        dst->v[dst->n]      = src->v[i];
        dst->v[dst->n].seq  = dst->seq++;
        dst->v[dst->n].core_index += core_base;
        dst->v[dst->n].destination_index = SIZE_MAX;
        if (dst->destination && dst->unit) {
            const ir_diag_t *source_diag = &src->v[i];
            const core_diagnostic_t *source_core =
                core_diagnostics_at(src->core, source_diag->core_index);
            if (source_core) {
                core_diag_location_t location = {0};
                if (ir_span_known(source_diag->primary.span)) {
                    ir_span_t span = source_diag->primary.span;
                    location = (core_diag_location_t){
                        .line = span.start_line,
                        .column = span.start_col,
                        .end_line = span.end_line,
                        .end_column = span.end_col,
                        .has_location = true,
                    };
                }
                size_t index = core_diagnostics_count(dst->destination);
                if (core_diagnostics_report_at(dst->destination, dst->unit,
                        core_severity_of(source_diag->severity),
                        core_string_from_cstr(
                            ir_diag_code(source_diag->code)),
                        source_core->message, location)
                    && core_diagnostics_count(dst->destination) > index)
                    dst->v[dst->n].destination_index = index;
            }
        }
        dst->n++;
    }
    dst->anchored |= src->anchored;
    dst->sorted = false;

    return true;
}

static int cmp_diag(const void *a, const void *b)
{
    const ir_diag_t *x = a, *y = b;
    bool             xk = ir_span_known(x->primary.span);
    bool             yk = ir_span_known(y->primary.span);

    if (xk != yk) {
        return xk ? -1 : 1;
    }

    if (xk) {
        if (x->primary.span.file != y->primary.span.file) {
            return x->primary.span.file < y->primary.span.file ? -1 : 1;
        }

        if (x->primary.span.start_byte != y->primary.span.start_byte) {
            return x->primary.span.start_byte < y->primary.span.start_byte
                       ? -1
                       : 1;
        }
    }

    if (x->severity != y->severity) {
        return x->severity < y->severity ? -1 : 1;
    }

    if (x->code != y->code) {
        return x->code < y->code ? -1 : 1;
    }

    return x->seq < y->seq ? -1 : x->seq > y->seq ? 1 : 0;
}

void ir_diags_sort(ir_diags_t *d)
{
    if (!d || d->sorted) {
        return;
    }
    qsort(d->v, d->n, sizeof(ir_diag_t), cmp_diag);
    d->sorted = true;
}
