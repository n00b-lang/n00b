#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/diag.h"

#define IR_DIAG(id, code, sev, text) {code, IR_SEV_##sev, text},

static const struct {
    const char   *code;
    ir_severity_t severity;
    const char   *text;
} codes[IR_DIAG_COUNT] = {
#include "ir/ir_diags.def"
};

#undef IR_DIAG

static bool seen[IR_DIAG_COUNT];

const char *ir_severity_name(ir_severity_t s)
{
    switch (s) {
    case IR_SEV_FATAL:
        return "fatal";
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

bool ir_diag_code_lookup(const char *code, ir_diagcode_t *out)
{
    if (!code) {
        return false;
    }

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        if (!strcmp(codes[i].code, code)) {
            if (out) {
                *out = (ir_diagcode_t)i;
            }

            return true;
        }
    }

    return false;
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
};

ir_policy_t *ir_policy_new(ir_ctx_t *ctx)
{
    if (!ctx) {
        return nullptr;
    }

    ir_policy_t *p = ir_alloc(ctx, sizeof(ir_policy_t));

    if (p) {
        p->ctx = ctx;
    }

    return p;
}

bool ir_policy_set(ir_policy_t *p, const char *key, ir_outcome_t outcome)
{
    if (!p || !key || !*key) {
        return false;
    }

    for (size_t i = 0; i < p->n; i++) {
        if (!strcmp(p->rules[i].key, key)) {
            p->rules[i].outcome = outcome;

            return true;
        }
    }

    p->rules = ir_grow(p->ctx, p->rules, &p->cap, p->n + 1,
                       sizeof(ir_rule_entry_t));

    if (!p->rules) {
        return false;
    }

    p->rules[p->n].key     = ir_strdup(p->ctx, key);
    p->rules[p->n].outcome = outcome;
    p->n++;

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
    ir_diag_t   *v;
    uint32_t     n;
    uint32_t     cap;
    uint64_t     seq;
    bool         fatal;
    bool         sorted;
};

ir_diags_t *ir_diags_new(ir_ctx_t *ctx, ir_policy_t *policy)
{
    if (!ctx) {
        return nullptr;
    }

    ir_diags_t *d = ir_alloc(ctx, sizeof(ir_diags_t));

    if (d) {
        d->ctx    = ctx;
        d->policy = policy;
        d->sorted = true;
    }

    return d;
}

static bool is_attachment(ir_severity_t s)
{
    return s == IR_SEV_NOTE || s == IR_SEV_HELP;
}

static bool apply_policy(ir_diags_t *d, ir_diagcode_t code, ir_severity_t *sev)
{
    if (*sev == IR_SEV_FATAL) {
        return true;
    }

    switch (ir_policy_for(d->policy, ir_diag_code(code))) {
    case IR_POLICY_ALLOW:
        return false;
    case IR_POLICY_WARN:
        if (!is_attachment(*sev)) {
            *sev = IR_SEV_WARNING;
        }
        break;
    case IR_POLICY_DENY:
    case IR_POLICY_FORBID:
        if (!is_attachment(*sev)) {
            *sev = IR_SEV_ERROR;
        }
        break;
    default:
        break;
    }

    return true;
}

static ir_diag_t *push(ir_diags_t *d, ir_diagcode_t code, ir_severity_t sev,
                       ir_span_t span, const char *label)
{
    if (!d || code >= IR_DIAG_COUNT) {
        return nullptr;
    }

    if (!ir_span_known(span) && !is_attachment(sev)) {
        return nullptr;
    }

    if (is_attachment(sev)) {
        bool anchored = false;

        for (uint32_t i = d->n; i > 0; i--) {
            if (!is_attachment(d->v[i - 1].severity)) {
                anchored = true;
                break;
            }
        }

        if (!anchored) {
            return nullptr;
        }
    }

    if (!apply_policy(d, code, &sev)) {
        return nullptr;
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

    seen[code]        = true;
    d->sorted         = false;

    if (sev == IR_SEV_FATAL) {
        d->fatal = true;
    }

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

    return dg->message != nullptr;
}

bool ir_diag_secondary(ir_diags_t *d, ir_diag_t *dg, ir_span_t span,
                       const char *label)
{
    if (!d || !dg) {
        return false;
    }

    size_t cap = dg->n_secondary;

    dg->secondary = ir_grow(d->ctx, dg->secondary, &cap, dg->n_secondary + 1u,
                            sizeof(ir_label_t));

    if (!dg->secondary) {
        return false;
    }

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

    size_t cap = dg->n_suggestions;

    dg->suggestions = ir_grow(d->ctx, dg->suggestions, &cap,
                              dg->n_suggestions + 1u,
                              sizeof(ir_suggestion_t));

    if (!dg->suggestions) {
        return false;
    }

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
    return d ? d->n : 0;
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
    if (!d) {
        return false;
    }

    for (uint32_t i = 0; i < d->n; i++) {
        if (d->v[i].severity == IR_SEV_ERROR
            || d->v[i].severity == IR_SEV_FATAL) {
            return true;
        }
    }

    return false;
}

bool ir_diags_fatal(const ir_diags_t *d)
{
    return d ? d->fatal : false;
}

uint32_t ir_diags_count_of(const ir_diags_t *d, ir_severity_t sev)
{
    if (!d) {
        return 0;
    }

    uint32_t n = 0;

    for (uint32_t i = 0; i < d->n; i++) {
        n += d->v[i].severity == sev;
    }

    return n;
}

bool ir_diags_merge(ir_diags_t *dst, ir_diags_t *src)
{
    if (!dst || !src) {
        return false;
    }

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
        dst->n++;
    }
    dst->fatal |= src->fatal;
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

void ir_diags_mark_seen(const ir_diags_t *d)
{
    if (!d) {
        return;
    }

    for (uint32_t i = 0; i < d->n; i++) {
        seen[d->v[i].code] = true;
    }
}

bool ir_diag_code_seen(ir_diagcode_t c)
{
    return c < IR_DIAG_COUNT && seen[c];
}

uint32_t ir_diag_unseen(const ir_diagcode_t **out)
{
    static ir_diagcode_t missing[IR_DIAG_COUNT];
    uint32_t             n = 0;

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        if (!seen[i]) {
            missing[n++] = (ir_diagcode_t)i;
        }
    }

    if (out) {
        *out = missing;
    }

    return n;
}
