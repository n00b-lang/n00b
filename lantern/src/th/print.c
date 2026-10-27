#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "private/th/internal.h"
#include "lib/list.h"
#include "lib/option.h"

enum { PRINT_BINDER_LIMIT = 32 };

typedef struct {
    th_ctx                   *ctx;
    size_t                    depth;
    const th_print_options_t *options;
    const th_type            *binders[PRINT_BINDER_LIMIT];
} pctx;

static bool source_spelling(const pctx *c)
{
    return c->options && c->options->source_spelling;
}

static bool integer_constraint(const th_type *var, bool *is_unsigned)
{
    for (size_t i = 0; var && i < var->var.cons.len; i++) {
        const th_constraint *k = &var->var.cons.data[i];
        if (k->kind != TH_CON_INTEGER) continue;
        if (is_unsigned) *is_unsigned = k->integer.flags & TH_INTEGER_UNSIGNED;
        return true;
    }
    return false;
}

static void source_variable(core_buffer_t *b, const th_type *var)
{
    bool is_unsigned = false;
    if (var && var->var.name.data) {
        core_buffer_putc(b, '\'');
        core_buffer_append_string(b, var->var.name);
    }
    else if (integer_constraint(var, &is_unsigned))
        core_buffer_append_string(b, is_unsigned
            ? CORE_STRING_STATIC("{unsigned integer}")
            : CORE_STRING_STATIC("{integer}"));
    else
        core_buffer_putc(b, '_');
}

static const th_type *bound_variable(const pctx *c, const th_type *bound)
{
    if (!bound->bound.distance || bound->bound.distance > c->depth
        || c->depth > PRINT_BINDER_LIMIT) return nullptr;
    const th_type *binder = c->binders[c->depth - bound->bound.distance];
    if (!binder || !binder->binder.qvars
        || bound->bound.position >= binder->binder.arity) return nullptr;
    return th_resolve_c(binder->binder.qvars[bound->bound.position]);
}

static void varname(core_buffer_t *b, size_t depth, uint64_t pos)
{
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz";
    uint64_t          n       = depth * 8 + pos;

    core_buffer_printf(b, "%c", alpha[n % 26]);

    if (n >= 26) {
        core_buffer_printf(b, "%" PRIu64, n / 26);
    }
}

static void emit(pctx *c, core_buffer_t *b, const th_type *t);

static const struct {
    core_string_t name;
    uint8_t       attrs;
} sugar[] = {
    {CORE_STRING_STATIC("ref"), 0x00},
    {CORE_STRING_STATIC("ptr"), TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE},
    {CORE_STRING_STATIC("mutref"), TH_EXCLUSIVE},
    {CORE_STRING_STATIC("own"), TH_OWNING},
    {CORE_STRING_STATIC("gcref"), TH_NULLABLE | TH_RESEATABLE},
    {CORE_STRING_STATIC("tmpref"), TH_TEMPORARY},
};

static const struct {
    core_string_t name;
    uint8_t       bit;
} attr_names[] = {
    {CORE_STRING_STATIC("nullable"), TH_NULLABLE},
    {CORE_STRING_STATIC("reseatable"), TH_RESEATABLE},
    {CORE_STRING_STATIC("indexable"), TH_INDEXABLE},
    {CORE_STRING_STATIC("owning"), TH_OWNING},
    {CORE_STRING_STATIC("exclusive"), TH_EXCLUSIVE},
    {CORE_STRING_STATIC("temporary"), TH_TEMPORARY},
    {CORE_STRING_STATIC("foreign"), TH_FOREIGN},
};

static core_string_t mode_name(const pctx *c, th_mode m)
{
    switch (m) {
    case TH_OUT:     return CORE_STRING_STATIC("out ");
    case TH_INOUT:   return source_spelling(c) ? CORE_STRING_STATIC("modify ")
                                               : CORE_STRING_STATIC("inout ");
    case TH_CONSUME: return CORE_STRING_STATIC("consume ");
    case TH_VIEW:    return CORE_STRING_STATIC("view ");
    case TH_IN:      return CORE_STRING_STATIC("");
    }
    return CORE_STRING_STATIC("");
}

typedef struct {
    char  *key;
    size_t keylen;
    size_t index;
} ordered;

static int cmp_ordered(const void *a, const void *b)
{
    const ordered *x = a, *y = b;
    th_slice       sx = {.p = (uint8_t *)x->key, .n = x->keylen};
    th_slice       sy = {.p = (uint8_t *)y->key, .n = y->keylen};

    return th_cmp_slice(&sx, &sy);
}

typedef enum {
    W_TYPE,
    W_LIT,
    W_ARG,
    W_ATTRS,
    W_SEQTAIL,
    W_DEPTH_DOWN,
} wkind;

typedef struct {
    wkind          kind;
    const th_type *t;
    const th_arg  *arg;
    core_string_t  s;
} witem;

core_option_decl(witem);
core_list_decl(witem);

static void wpush(core_list_t(witem) *w, witem it)
{
    core_list_push(*w, it);
}

static void wtext(core_list_t(witem) *w, core_string_t s)
{
    wpush(w, (witem){.kind = W_LIT, .s = s});
}

#define wlit(w, lit) wtext((w), CORE_STRING_STATIC(lit))

static void wtype(core_list_t(witem) *w, const th_type *t)
{
    wpush(w, (witem){.kind = W_TYPE, .t = t});
}

static void emit_arg_scalar(core_buffer_t *b, const th_arg *a)
{
    switch (a->kind) {
    case TH_ARG_UINT:
        core_buffer_printf(b, "%" PRIu64, a->u);
        break;
    case TH_ARG_SINT:
        core_buffer_printf(b, "%" PRId64, a->s);
        break;
    case TH_ARG_STR:
        core_buffer_puts(b, "\"");
        core_buffer_append_string(b, a->str);
        core_buffer_puts(b, "\"");
        break;
    default:
        break;
    }
}

static void emit_attrs(core_buffer_t *b, const th_type *t)
{
    core_buffer_puts(b, "; ");

    bool first = true;

    for (size_t i = 0; i < sizeof(attr_names) / sizeof(attr_names[0]); i++) {
        if (t->ind.attrs & attr_names[i].bit) {
            if (!first) {
                core_buffer_puts(b, ", ");
            }
            core_buffer_append_string(b, attr_names[i].name);
            first = false;
        }
    }
    core_buffer_puts(b, "]");
}

static void emit_seqtail(core_buffer_t *b, const th_type *t)
{
    if (t->seq.has_extent) {
        core_buffer_printf(b, "; %" PRIu64 "]", t->seq.extent);
    }
    else {
        core_buffer_puts(b, "]");
    }
}

static size_t *sorted_members(pctx *c, const th_type *t)
{
    size_t   n = t->record.n;
    ordered *o = core_alloc_array(ordered, n);
    size_t  *ix = core_alloc_array(size_t, n);

    for (size_t i = 0; i < n; i++) {
        core_string_t nm  = t->record.members[i].name;
        size_t        len = nm.u8_bytes;
        uint8_t       hdr[8];
        core_buffer_t key = {0};
        core_buffer_t enc = {0};

        for (int k = 0; k < 8; k++) {
            hdr[k] = (uint8_t)(len >> (56 - 8 * k));
        }
        core_buffer_append(&key, (const char *)hdr, 8);
        core_buffer_append_string(&key, nm);

        th_full_encode(c->ctx, t->record.members[i].type, &enc);
        core_buffer_append(&key, enc.data, enc.byte_len);
        core_buffer_release(&enc);

        o[i].key    = key.data;
        o[i].keylen = key.byte_len;
        o[i].index  = i;
    }
    qsort(o, n, sizeof(ordered), cmp_ordered);

    for (size_t i = 0; i < n; i++) {
        ix[i] = o[i].index;
        core_free(o[i].key);
    }
    core_free(o);

    return ix;
}

static size_t *sorted_arms(pctx *c, const th_type *t)
{
    size_t   n = t->sum.n;
    ordered *o = core_alloc_array(ordered, n);
    size_t  *ix = core_alloc_array(size_t, n);

    for (size_t i = 0; i < n; i++) {
        core_buffer_t enc = {0};

        th_full_encode(c->ctx, t->sum.arms[i], &enc);
        o[i].key    = enc.data;
        o[i].keylen = enc.byte_len;
        o[i].index  = i;
    }
    qsort(o, n, sizeof(ordered), cmp_ordered);

    for (size_t i = 0; i < n; i++) {
        ix[i] = o[i].index;
        core_free(o[i].key);
    }
    core_free(o);

    return ix;
}

static void plan_record(pctx *c, core_buffer_t *b, core_list_t(witem) *w, const th_type *t)
{
    size_t  n  = t->record.n;
    size_t *ix = sorted_members(c, t);

    core_buffer_append_string(b, source_spelling(c)
        ? CORE_STRING_STATIC("Tuple[") : CORE_STRING_STATIC("("));
    wtext(w, source_spelling(c) ? CORE_STRING_STATIC("]")
                                : CORE_STRING_STATIC(")"));

    for (size_t k = n; k-- > 0;) {
        const th_member *m = &t->record.members[ix[k]];

        wtype(w, m->type);
        wlit(w, ": ");
        wtext(w, m->name);

        if (k) {
            wlit(w, ", ");
        }
    }
    core_free(ix);
}

static void plan_sum(pctx *c, core_buffer_t *b, core_list_t(witem) *w, const th_type *t)
{
    size_t      n  = t->sum.n;
    size_t     *ix = sorted_arms(c, t);
    core_string_t op = t->sum.comb == TH_UNION ? CORE_STRING_STATIC(" | ")
                                               : CORE_STRING_STATIC(" & ");

    core_buffer_puts(b, "(");
    wlit(w, ")");

    for (size_t k = n; k-- > 0;) {
        wtype(w, t->sum.arms[ix[k]]);

        if (k) {
            wtext(w, op);
        }
    }
    core_free(ix);
}

static void plan_function(pctx *c, core_buffer_t *b, core_list_t(witem) *w, const th_type *t)
{
    th_kwparam *kw = nullptr;

    if (t->fn.nkw) {
        kw = core_alloc_array(th_kwparam, t->fn.nkw);
        memcpy(kw, t->fn.kw, t->fn.nkw * sizeof(th_kwparam));
        qsort(kw, t->fn.nkw, sizeof(th_kwparam), th_cmp_kw);
    }

    core_buffer_puts(b, "(");

    if (t->fn.error && !(c->options && c->options->omit_nil_failure
                         && th_is_nil(t->fn.error))) {
        wtype(w, t->fn.error);
        wlit(w, " fail: ");
    }
    wtype(w, t->fn.ret);
    wlit(w, ") -> ");

    size_t slot = t->fn.np + (t->fn.rest ? 1 : 0) + t->fn.nkw;

    for (size_t k = t->fn.nkw; k-- > 0;) {
        wtype(w, kw[k].type);
        wtext(w, mode_name(c, kw[k].mode));
        wlit(w, ": ");
        wtext(w, kw[k].name);

        if (--slot) {
            wlit(w, ", ");
        }
    }
    core_free(kw);

    if (t->fn.rest) {
        wtype(w, t->fn.rest->type);
        if (source_spelling(c)) {
            wtext(w, mode_name(c, t->fn.rest->mode));
            wlit(w, "*");
        }
        else {
            wlit(w, "*");
            wtext(w, mode_name(c, t->fn.rest->mode));
        }

        if (--slot) {
            wlit(w, ", ");
        }
    }

    for (size_t k = t->fn.np; k-- > 0;) {
        wtype(w, t->fn.params[k].type);
        wtext(w, mode_name(c, t->fn.params[k].mode));

        if (--slot) {
            wlit(w, ", ");
        }
    }
}

static void expand(pctx *c, core_buffer_t *b, core_list_t(witem) *w, const th_type *t)
{
    t = th_resolve_c(t);

    if (!t) {
        core_buffer_puts(b, "?");

        return;
    }

    switch (t->kind) {
    case TH_NAMED: {
        bool all_types = true;

        for (size_t i = 0; i < t->named.nargs; i++) {
            all_types &= t->named.args[i].kind == TH_ARG_TYPE;
        }

        if (!t->named.is_variant && all_types
            && core_string_eq(t->named.name, CORE_STRING_STATIC(TH_TUPLE))) {
            core_buffer_append_string(b, source_spelling(c)
                ? CORE_STRING_STATIC("Tuple[") : CORE_STRING_STATIC("t("));
            wtext(w, source_spelling(c) ? CORE_STRING_STATIC("]")
                                        : CORE_STRING_STATIC(")"));

            for (size_t k = t->named.nargs; k-- > 0;) {
                wtype(w, t->named.args[k].type);

                if (k) {
                    wlit(w, ", ");
                }
            }

            return;
        }

        th_type *option_item = th_option_item(t);
        if (c->options && c->options->option_suffix.data && option_item) {
            wtext(w, c->options->option_suffix);
            wtype(w, option_item);
            return;
        }
        if (t->named.is_variant && !source_spelling(c)) {
            core_buffer_puts(b, "variant ");
        }
        core_string_t mapped = c->options && c->options->named
            ? c->options->named(t->named.name, c->options->context)
            : (core_string_t){};
        core_buffer_append_string(b, mapped.data ? mapped : t->named.name);

        if (!t->named.nargs) {
            return;
        }
        core_buffer_puts(b, "[");
        wlit(w, "]");

        for (size_t k = t->named.nargs; k-- > 0;) {
            wpush(w, (witem){.kind = W_ARG, .arg = &t->named.args[k]});

            if (k) {
                wlit(w, ", ");
            }
        }

        return;
    }

    case TH_BITINT:
        if (source_spelling(c))
            core_buffer_printf(b, "%s:%" PRIu64,
                               t->bitint.flavor == TH_BITINT_BITS ? "bits"
                               : t->bitint.flavor == TH_BITINT_SIGNED ? "i"
                                                                      : "u",
                               t->bitint.width);
        else if (t->bitint.flavor == TH_BITINT_BITS)
            core_buffer_printf(b, "bits:%" PRIu64, t->bitint.width);
        else
            core_buffer_printf(b, t->bitint.flavor == TH_BITINT_SIGNED
                                      ? "bitint[%" PRIu64 "]"
                                      : "ubitint[%" PRIu64 "]",
                               t->bitint.width);

        return;

    case TH_INDIRECTION:
        for (size_t i = 0; i < sizeof(sugar) / sizeof(sugar[0]); i++) {
            if (sugar[i].attrs == t->ind.attrs) {
                core_string_t name = !t->ind.attrs && c->options
                    && c->options->reference.data
                    ? c->options->reference : sugar[i].name;
                core_buffer_append_string(b, name);
                core_buffer_puts(b, "[");
                wlit(w, "]");
                wtype(w, t->ind.referent);

                return;
            }
        }
        core_buffer_append_string(b,
            c->options && c->options->reference.data
                ? c->options->reference : CORE_STRING_STATIC("ref"));
        core_buffer_puts(b, "[");
        wpush(w, (witem){.kind = W_ATTRS, .t = t});
        wtype(w, t->ind.referent);

        return;

    case TH_SEQUENCE:
        core_buffer_append_string(b,
            c->options && c->options->sequence.data
                ? c->options->sequence : CORE_STRING_STATIC("array"));
        core_buffer_puts(b, "[");
        wpush(w, (witem){.kind = W_SEQTAIL, .t = t});
        wtype(w, t->seq.elem);

        return;

    case TH_RECORD:
        plan_record(c, b, w, t);

        return;

    case TH_TRAIT:
        core_buffer_puts(b, "trait ");
        core_buffer_append_string(b, t->trait.name);
        return;

    case TH_FUNCTION:
        plan_function(c, b, w, t);

        return;

    case TH_BINDER:
        if (source_spelling(c)) {
            if (c->depth < PRINT_BINDER_LIMIT) c->binders[c->depth] = t;
            c->depth++;
            wpush(w, (witem){.kind = W_DEPTH_DOWN});
            wtype(w, t->binder.body);
            return;
        }
        core_buffer_puts(b, "forall[");

        for (uint64_t i = 0; i < t->binder.arity; i++) {
            if (i) {
                core_buffer_puts(b, ", `");
            }
            else {
                core_buffer_puts(b, "`");
            }
            varname(b, c->depth, i);
        }
        core_buffer_puts(b, "] ");
        c->depth++;
        wpush(w, (witem){.kind = W_DEPTH_DOWN});
        wtype(w, t->binder.body);

        return;

    case TH_BOUND:
        if (source_spelling(c)) {
            source_variable(b, bound_variable(c, t));
            return;
        }
        core_buffer_puts(b, "`");

        if (t->bound.distance == 0 || t->bound.distance > c->depth) {
            core_buffer_printf(b, "?%" PRIu64, t->bound.position);

            return;
        }
        varname(b, c->depth - t->bound.distance, t->bound.position);

        return;

    case TH_VAR:
        if (source_spelling(c)) {
            source_variable(b, t);
            return;
        }
        core_buffer_puts(b, "`");

        if (!t->var.has_index) {
            core_buffer_puts(b, "?");

            return;
        }
        varname(b, 0, t->var.index);

        return;

    case TH_SUM:
        plan_sum(c, b, w, t);

        return;

    default:
        core_buffer_puts(b, "?");
    }
}

static void emit(pctx *c, core_buffer_t *b, const th_type *root)
{
    core_list_t(witem) w = core_list_new(witem);

    wtype(&w, root);

    while (w.len) {
        witem it = core_option_get(core_list_pop(witem, w));

        switch (it.kind) {
        case W_LIT:
            core_buffer_append_string(b, it.s);
            break;
        case W_DEPTH_DOWN:
            c->depth--;
            break;
        case W_ATTRS:
            emit_attrs(b, it.t);
            break;
        case W_SEQTAIL:
            emit_seqtail(b, it.t);
            break;
        case W_ARG:
            if (it.arg->kind == TH_ARG_TYPE) {
                wtype(&w, it.arg->type);
            }
            else {
                emit_arg_scalar(b, it.arg);
            }
            break;
        case W_TYPE:
            expand(c, b, &w, it.t);
            break;
        }
    }

    core_list_free(w);
}

static void emit_constraint(pctx *c, core_buffer_t *b, const th_constraint *k)
{
    switch (k->kind) {
    case TH_CON_KIND:
        core_buffer_printf(b, "kind(%u)", (unsigned)k->type_kind);
        break;
    case TH_CON_INTEGER:
        if (k->integer.flags & TH_INTEGER_UNSIGNED)
            core_buffer_puts(b, "integer(unsigned)");
        else
            core_buffer_puts(b, "integer");
        break;
    case TH_CON_IMPLEMENTS:
        core_buffer_append_string(b, k->iface);
        break;

    case TH_CON_UNIFIES:
        core_buffer_puts(b, "== ");
        emit(c, b, k->target);
        break;

    case TH_CON_EXCLUDES:
        core_buffer_puts(b, "!= ");
        emit(c, b, k->target);
        break;

    case TH_CON_PROMOTES:
        core_buffer_puts(b, "promotes_to ");
        emit(c, b, k->target);
        break;

    case TH_CON_ONE_OF:
        core_buffer_puts(b, "one_of(");

        for (size_t i = 0; i < k->one_of.n; i++) {
            if (i) {
                core_buffer_puts(b, ", ");
            }
            emit(c, b, k->one_of.types[i]);
        }
        core_buffer_puts(b, ")");
        break;

    case TH_CON_HAS_FIELD:
        if (k->field.auto_deref) {
            core_buffer_puts(b, "has_field_auto_deref(");
        }
        else {
            core_buffer_puts(b, "has_field(");
        }
        core_buffer_append_string(b, k->field.name);
        core_buffer_puts(b, ": ");
        emit(c, b, k->field.type);
        core_buffer_puts(b, ")");
        break;

    case TH_CON_HAS_PARAM:
        core_buffer_printf(b, "has_param(%llu, ",
                          (unsigned long long)k->param.index);
        emit(c, b, k->param.type);
        core_buffer_puts(b, ")");
        break;

    case TH_CON_METHOD:
        core_buffer_puts(b, "method(");
        core_buffer_append_string(b, k->method.name);
        core_buffer_puts(b, ": ");
        emit(c, b, k->method.result);
        if (k->method.call) {
            core_buffer_puts(b, ", args: ");
            emit(c, b, k->method.call);
        }
        core_buffer_puts(b, ")");
        break;
    }
}

static void emit_where(pctx *c, core_buffer_t *b, th_type *type)
{
    const th_type *r = th_resolve_c(type);

    if (!r || r->kind != TH_VAR || !r->var.cons.len) {
        return;
    }

    core_buffer_puts(b, " where `");
    core_buffer_append_string(b, r->var.name.data ? r->var.name
                                                  : CORE_STRING_STATIC("t"));
    core_buffer_puts(b, ": ");

    for (size_t i = 0; i < r->var.cons.len; i++) {
        if (i) {
            core_buffer_puts(b, " + ");
        }
        emit_constraint(c, b, &r->var.cons.data[i]);
    }
}

static void emit_free_where(pctx *c, core_buffer_t *b, th_type *type)
{
    const th_type *root = th_resolve_c(type);
    if (!root || root->kind == TH_BINDER) return;
    if (root->kind == TH_VAR) {
        emit_where(c, b, type);
        return;
    }

    th_type **vars = nullptr;
    size_t nvars = th_free_vars(c->ctx, type, &vars);
    bool started = false;

    for (size_t i = 0; i < nvars; i++) {
        const th_type *var = th_resolve_c(vars[i]);
        if (!var || var->kind != TH_VAR || !var->var.cons.len) continue;

        if (started) core_buffer_puts(b, ", `");
        else core_buffer_puts(b, " where `");
        if (var->var.has_index) varname(b, 0, var->var.index);
        else core_buffer_append_string(b, var->var.name.data
                                              ? var->var.name
                                              : CORE_STRING_STATIC("t"));
        core_buffer_puts(b, ": ");
        for (size_t j = 0; j < var->var.cons.len; j++) {
            if (j) core_buffer_puts(b, " + ");
            emit_constraint(c, b, &var->var.cons.data[j]);
        }
        started = true;
    }
}

static void emit_binder_where(pctx *c, core_buffer_t *b, th_type *type)
{
    const th_type *scheme = th_resolve_c(type);
    if (!scheme || scheme->kind != TH_BINDER || !scheme->binder.qvars) return;
    const th_type *body = th_resolve_c(scheme->binder.body);
    if (!body || body->kind != TH_FUNCTION) return;

    bool started = false;
    for (uint64_t i = 0; i < scheme->binder.arity; i++) {
        const th_type *var = th_resolve_c(scheme->binder.qvars[i]);
        if (!var || var->kind != TH_VAR || !var->var.cons.len) continue;
        if (!started) {
            core_buffer_puts(b, " where ");
            started = true;
        }
        else core_buffer_puts(b, ", ");
        core_buffer_putc(b, '`');
        varname(b, 0, i);
        core_buffer_puts(b, ": ");
        for (size_t j = 0; j < var->var.cons.len; j++) {
            if (j) core_buffer_puts(b, " + ");
            emit_constraint(c, b, &var->var.cons.data[j]);
        }
    }
}

static void emit_source_requirements(core_buffer_t *b, const th_type *var,
                                     bool *started)
{
    var = th_resolve_c(var);
    if (!var || var->kind != TH_VAR || !var->var.name.data) return;
    bool listed = false;
    for (size_t i = 0; i < var->var.cons.len; i++) {
        const th_constraint *k = &var->var.cons.data[i];
        if (k->kind != TH_CON_IMPLEMENTS || !k->iface.data) continue;
        if (!listed) {
            core_buffer_append_string(b, *started
                ? CORE_STRING_STATIC(", ") : CORE_STRING_STATIC(" where "));
            source_variable(b, var);
            core_buffer_puts(b, ": ");
        }
        else core_buffer_puts(b, " + ");
        core_buffer_append_string(b, k->iface);
        listed = true;
        *started = true;
    }
}

static void emit_source_where(pctx *c, core_buffer_t *b, th_type *type)
{
    const th_type *root = th_resolve_c(type);
    if (!root) return;
    bool started = false;
    if (root->kind == TH_BINDER) {
        for (uint64_t i = 0; root->binder.qvars && i < root->binder.arity;
             i++)
            emit_source_requirements(b, root->binder.qvars[i], &started);
        return;
    }
    th_type **vars = nullptr;
    size_t nvars = th_free_vars(c->ctx, type, &vars);
    for (size_t i = 0; i < nvars; i++)
        emit_source_requirements(b, vars[i], &started);
}

core_string_t
th_print_with_options(th_ctx *ctx, th_type *type,
                      const th_print_options_t *options)
{
    if (!ctx || !type) {
        return (core_string_t){};
    }
    th_normalize(type);

    pctx  c = {.ctx = ctx, .options = options};
    core_buffer_t b = {0};

    emit(&c, &b, type);
    if (options && options->source_spelling) {
        if (!options->omit_constraints) emit_source_where(&c, &b, type);
    }
    else if (!options || !options->omit_constraints) {
        emit_free_where(&c, &b, type);
        emit_binder_where(&c, &b, type);
    }

    if (!b.data) {
        return th_strdup(ctx, CORE_STRING_STATIC(""));
    }

    core_string_t out = th_strdup(ctx, core_buffer_string(&b));

    core_buffer_release(&b);

    return out;
}

core_string_t th_print(th_ctx *ctx, th_type *type)
{
    return th_print_with_options(ctx, type, nullptr);
}
