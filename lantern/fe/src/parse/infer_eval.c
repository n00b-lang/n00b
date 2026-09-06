#include "parse/infer.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "ir/typelang.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "typehash.h"

#include <string.h>

#define INFER_MAX_TVARS 16

typedef struct {
    ncc_string_t name;
    th_type     *var;
} tvar_t;

typedef struct {
    th_ctx       *tc;
    ncc_symtab_t *st;
    ncc_visit_t  *v;
    tvar_t        tvars[INFER_MAX_TVARS];
    size_t        n_tvars;
    bool          failed;
} ictx;

static th_type *eval(ictx *c, const ncc_infer_expr_t *e);

static th_type *
fresh(ictx *c)
{
    return th_var(c->tc);
}

static ncc_parse_tree_t *
child(ictx *c, int32_t ix)
{
    if (ix < 0 || (size_t)ix >= ncc_tree_num_children(c->v->node)) {
        return nullptr;
    }

    return ncc_tree_child(c->v->node, (size_t)ix);
}

static th_type *
type_of(ictx *c, int32_t ix)
{
    ncc_parse_tree_t *k = child(c, ix);

    if (!k || ncc_tree_is_leaf(k)) {
        return nullptr;
    }

    ncc_nt_node_t *pn = &ncc_tree_node_value(k);

    if (!pn->type) {
        pn->type = fresh(c);
    }

    return pn->type;
}

static th_type *
tvar(ictx *c, ncc_string_t name)
{
    for (size_t i = 0; i < c->n_tvars; i++) {
        if (ncc_string_eq(c->tvars[i].name, name)) {
            return c->tvars[i].var;
        }
    }

    th_type *t = th_var_named(c->tc, name.data ? name.data : "t");

    if (c->n_tvars < INFER_MAX_TVARS) {
        c->tvars[c->n_tvars].name = name;
        c->tvars[c->n_tvars].var  = t;
        c->n_tvars++;
    }

    return t;
}

static th_type *
symbol_type(ictx *c, int32_t ix)
{
    ncc_parse_tree_t *k = child(c, ix);
    ncc_string_t      nm = k ? ncc_tree_extract_first_identifier(k)
                             : (ncc_string_t){0};

    if (!nm.u8_bytes || !c->st) {
        return nullptr;
    }

    ncc_scope_t *scope = c->v->pn->scope
                           ? c->v->pn->scope
                           : ncc_symtab_current_scope(c->st,
                                                      ncc_string_empty());
    ncc_sym_entry_t *sym = ncc_symtab_lookup_scoped(c->st, scope,
                                                    ncc_string_empty(), nm);

    if (!sym) {
        return nullptr;
    }

    if (!sym->type) {
        sym->type = fresh(c);
    }

    return sym->type;
}

static th_type *
named_arg_last(const th_type *t)
{
    size_t n = th_named_nargs(t);

    if (!n) {
        return nullptr;
    }

    const th_arg *a = th_named_arg(t, n - 1);

    return a && a->kind == TH_ARG_TYPE ? a->type : nullptr;
}

static th_type *
named_arg_first(const th_type *t)
{
    if (!th_named_nargs(t)) {
        return nullptr;
    }

    const th_arg *a = th_named_arg(t, 0);

    return a && a->kind == TH_ARG_TYPE ? a->type : nullptr;
}

static bool
named_is(const th_type *t, const char *want)
{
    const char *nm    = nullptr;
    size_t      nlen  = 0;
    size_t      nargs = 0;
    bool        var   = false;

    if (!th_named_info(t, &nm, &nlen, &var, &nargs) || !nm) {
        return false;
    }

    return nlen == strlen(want) && !memcmp(nm, want, nlen);
}

static th_type *
project(ictx *c, ncc_infer_proj_t op, int32_t ix)
{
    th_type *t = type_of(c, ix);

    if (!t) {
        return fresh(c);
    }

    switch (op) {
    case NCC_INFER_RETURN_OF: {
        th_type *r = th_func_ret(t);

        return r ? r : t;
    }

    case NCC_INFER_ELEMENT_OF: {
        th_type *e = th_seq_elem(t);

        if (e) {
            return e;
        }

        if (named_is(t, "dict")) {
            e = named_arg_last(t);
        }
        else {
            e = named_arg_first(t);
        }

        return e ? e : fresh(c);
    }

    case NCC_INFER_UNWRAP_RESULT: {
        if (named_is(t, "result") || named_is(t, "option")) {
            th_type *inner = named_arg_first(t);

            if (inner) {
                return inner;
            }
        }

        th_type *inner = fresh(c);
        th_arg   a     = {.kind = TH_ARG_TYPE, .type = inner};

        th_unify(c->tc, t, th_apply(c->tc, "result", &a, 1));

        return inner;
    }

    case NCC_INFER_METHOD_RETURN:
        return fresh(c);
    }

    return fresh(c);
}

static size_t
spread_args(ictx *c, int32_t ix, th_arg *out, size_t max)
{
    ncc_parse_tree_t *node = child(c, ix);
    size_t            n    = 0;

    if (!node) {
        return 0;
    }

    size_t nc = ncc_tree_num_children(node);

    for (size_t i = 0; i < nc && n < max; i++) {
        ncc_parse_tree_t *k = ncc_tree_child(node, i);

        if (!k || ncc_tree_is_leaf(k)) {
            continue;
        }

        th_type *t = ir_type_from_node(c->tc, c->v->grammar, k);

        if (t) {
            out[n].kind = TH_ARG_TYPE;
            out[n].type = t;
            n++;
        }
    }

    return n;
}

#define INFER_MAX_ARGS 16

static size_t
app_args(ictx *c, const ncc_infer_expr_t *e, th_arg *out, size_t max)
{
    size_t n = 0;

    for (size_t i = 0; i < e->n_args && n < max; i++) {
        const ncc_infer_expr_t *a = e->args[i];

        if (a->kind == NCC_INFER_SPREAD) {
            n += spread_args(c, a->index, out + n, max - n);

            continue;
        }

        th_type *t = eval(c, a);

        if (!t) {
            c->failed = true;

            return n;
        }

        out[n].kind = TH_ARG_TYPE;
        out[n].type = t;
        n++;
    }

    return n;
}

static th_type *
eval(ictx *c, const ncc_infer_expr_t *e)
{
    if (!e) {
        return nullptr;
    }

    switch (e->kind) {
    case NCC_INFER_CHILD: {
        th_type *t = type_of(c, e->index);

        return t ? t : fresh(c);
    }

    case NCC_INFER_SELF:
        if (!c->v->pn->type) {
            c->v->pn->type = fresh(c);
        }

        return c->v->pn->type;

    case NCC_INFER_RETURN: {
        ncc_sym_entry_t *sym
            = c->st ? ncc_symtab_lookup(c->st, ncc_string_empty(),
                                        NCC_STRING_STATIC("$return"))
                    : nullptr;

        if (sym) {
            if (!sym->type) {
                sym->type = fresh(c);
            }

            return sym->type;
        }

        return fresh(c);
    }

    case NCC_INFER_TVAR:
        return tvar(c, e->name);

    case NCC_INFER_NAMED:
        return th_name(c->tc, e->name.data ? e->name.data : "");

    case NCC_INFER_NAMED_APP: {
        th_arg args[INFER_MAX_ARGS];
        size_t n = app_args(c, e, args, INFER_MAX_ARGS);

        return th_apply(c->tc, e->name.data ? e->name.data : "", args, n);
    }

    case NCC_INFER_CHILD_APP: {
        ncc_parse_tree_t *k  = child(c, e->index);
        ncc_string_t      nm = k ? ncc_tree_extract_first_identifier(k)
                                 : (ncc_string_t){0};

        if (!nm.u8_bytes) {
            c->failed = true;

            return nullptr;
        }

        th_arg args[INFER_MAX_ARGS];
        size_t n = app_args(c, e, args, INFER_MAX_ARGS);

        return th_apply(c->tc, nm.data, args, n);
    }

    case NCC_INFER_SPREAD:
        c->failed = true;

        return nullptr;

    case NCC_INFER_LOOKUP: {
        th_type *t = symbol_type(c, e->index);

        return t ? t : fresh(c);
    }

    case NCC_INFER_PROJECT:
        return project(c, e->proj, e->index);

    case NCC_INFER_UNIFY: {
        th_type *l = eval(c, e->lhs);
        th_type *r = eval(c, e->rhs);

        if (!l || !r) {
            c->failed = true;

            return l;
        }

        th_unify(c->tc, l, r);

        return l;
    }

    case NCC_INFER_SUM: {
        th_type *l = eval(c, e->lhs);
        th_type *r = eval(c, e->rhs);

        if (!l || !r) {
            c->failed = true;

            return l;
        }

        th_type *arms[2] = {l, r};

        return th_sum(c->tc, TH_UNION, arms, 2);
    }
    }

    return nullptr;
}

th_type *
ncc_infer_eval(const ncc_infer_expr_t *e,
               ncc_visit_t            *v,
               th_ctx                 *tc,
               ncc_symtab_t           *st,
               bool                   *ok)
{
    if (ok) {
        *ok = false;
    }

    if (!e || !v || !tc) {
        return nullptr;
    }

    ictx c = {.tc = tc, .st = st, .v = v};

    th_type *t = eval(&c, e);

    if (ok) {
        *ok = !c.failed && t != nullptr;
    }

    return t;
}
