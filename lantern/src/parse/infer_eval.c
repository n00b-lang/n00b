#include "parse/infer.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "ir/typelang.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/list.h"
#include "th/typehash.h"

#include <string.h>

typedef struct {
    core_string_t name;
    th_type     *var;
} tvar_t;

core_list_decl(tvar_t);
core_list_decl(th_arg);

typedef struct {
    th_ctx       *tc;
    parse_symtab_t *st;
    parse_visit_t  *v;
    core_list_t(tvar_t) tvars;
    bool          failed;
} ictx;

static th_type *eval(ictx *c, const parse_infer_expr_t *e);

static th_type *
fresh(ictx *c)
{
    return th_var(c->tc);
}

static parse_tree_t *
child(ictx *c, int32_t ix)
{
    if (ix < 0 || (size_t)ix >= core_tree_num_children(c->v->node)) {
        return nullptr;
    }

    return core_tree_child(c->v->node, (size_t)ix);
}

static th_type *
type_of(ictx *c, int32_t ix)
{
    parse_tree_t *k = child(c, ix);

    if (!k || core_tree_is_leaf(k)) {
        return nullptr;
    }

    parse_nt_node_t *pn = &core_tree_node_value(k);

    if (!pn->type) {
        pn->type = fresh(c);
    }

    return pn->type;
}

static th_type *
tvar(ictx *c, core_string_t name)
{
    for (size_t i = 0; i < c->tvars.len; i++) {
        if (core_string_eq(c->tvars.data[i].name, name)) {
            return c->tvars.data[i].var;
        }
    }

    th_type *t = th_var_named(c->tc, name.data ? name.data : "t");

    core_list_push(c->tvars, ((tvar_t){.name = name, .var = t}));

    return t;
}

static th_type *
symbol_type(ictx *c, int32_t ix)
{
    parse_tree_t *k = child(c, ix);
    core_string_t      nm = k ? parse_tree_extract_first_identifier(k)
                             : (core_string_t){0};

    if (!nm.u8_bytes || !c->st) {
        return nullptr;
    }

    parse_scope_t *scope = c->v->pn->scope
                           ? c->v->pn->scope
                           : parse_symtab_current_scope(c->st,
                                                      core_string_empty());
    parse_sym_entry_t *sym = parse_symtab_lookup_scoped(c->st, scope,
                                                    core_string_empty(), nm);

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

static bool
leaf_has_text(parse_tree_t *leaf, void *arg)
{
    (void)arg;

    parse_token_info_t *tok = core_tree_leaf_value(leaf);

    return tok && core_option_is_set(tok->value)
        && core_option_get(tok->value).u8_bytes;
}

static core_string_t
last_text(parse_tree_t *node)
{
    parse_tree_t *leaf = parse_tree_scan_leaves(node, true, leaf_has_text,
                                                 nullptr);

    return leaf ? core_option_get(core_tree_leaf_value(leaf)->value)
                : (core_string_t){0};
}

static bool
text_is(core_string_t text, const char *want)
{
    size_t n = strlen(want);

    return text.u8_bytes == n && !memcmp(text.data, want, n);
}

static th_type *
project(ictx *c, parse_infer_proj_t op, int32_t ix)
{
    th_type *t = type_of(c, ix);

    if (!t) {
        return fresh(c);
    }

    switch (op) {
    case PARSE_INFER_RETURN_OF: {
        th_type *r = th_func_ret(t);

        if (r) {
            return r;
        }

        th_kind kind = th_kind_of(t);

        return kind == TH_NAMED || kind == TH_RECORD ? t : fresh(c);
    }

    case PARSE_INFER_ELEMENT_OF: {
        th_type *e = th_seq_elem(t);

        if (e) {
            return e;
        }

        if (named_is(t, "dict")) {
            e = named_arg_last(t);
        }
        else if (named_is(t, "string")) {
            e = t;
        }
        else {
            e = named_arg_first(t);
        }

        return e ? e : fresh(c);
    }

    case PARSE_INFER_UNWRAP_RESULT: {
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

    case PARSE_INFER_METHOD_RETURN: {
        core_string_t name = last_text(c->v->node);

        if (!name.data) {
            return fresh(c);
        }

        if (named_is(t, "option") || named_is(t, "result")) {
            if (text_is(name, "is_set?") || text_is(name, "ok?")
                || text_is(name, "is_ok?")) {
                return th_name(c->tc, "bool");
            }
            if (text_is(name, "unwrap")) {
                th_type *inner = named_arg_first(t);

                return inner ? inner : fresh(c);
            }
        }

        char *method_name = core_alloc_array(char, name.u8_bytes + 1);

        memcpy(method_name, name.data, name.u8_bytes);
        method_name[name.u8_bytes] = 0;

        th_type *signature = th_method_lookup(c->tc, t, method_name);

        if (!signature) {
            signature = th_record_lookup(t, method_name);
        }

        th_type *ret = signature ? th_func_ret(signature) : nullptr;

        core_free(method_name);

        return ret ? ret : fresh(c);
    }
    }

    return fresh(c);
}

static void
spread_args(ictx *c, int32_t ix, core_list_t(th_arg) * out)
{
    parse_tree_t *node = child(c, ix);

    if (!node) {
        return;
    }

    size_t nc = core_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        parse_tree_t *k = core_tree_child(node, i);

        if (!k || core_tree_is_leaf(k)) {
            continue;
        }

        th_type *t = ir_type_from_node(c->tc, c->v->grammar, k);

        if (t) {
            core_list_push(*out,
                           ((th_arg){.kind = TH_ARG_TYPE, .type = t}));
        }
    }
}

static void
app_args(ictx *c, const parse_infer_expr_t *e, core_list_t(th_arg) * out)
{
    for (size_t i = 0; i < e->n_args; i++) {
        const parse_infer_expr_t *a = e->args[i];

        if (a->kind == PARSE_INFER_SPREAD) {
            spread_args(c, a->index, out);

            continue;
        }

        th_type *t = eval(c, a);

        if (!t) {
            c->failed = true;

            return;
        }

        core_list_push(*out, ((th_arg){.kind = TH_ARG_TYPE, .type = t}));
    }
}

static th_type *
eval(ictx *c, const parse_infer_expr_t *e)
{
    if (!e) {
        return nullptr;
    }

    switch (e->kind) {
    case PARSE_INFER_CHILD: {
        th_type *t = type_of(c, e->index);

        return t ? t : fresh(c);
    }

    case PARSE_INFER_SELF:
        if (!c->v->pn->type) {
            c->v->pn->type = fresh(c);
        }

        return c->v->pn->type;

    case PARSE_INFER_RETURN: {
        parse_sym_entry_t *sym
            = c->st ? parse_symtab_lookup(c->st, core_string_empty(),
                                        CORE_STRING_STATIC("$return"))
                    : nullptr;

        if (sym) {
            if (!sym->type) {
                sym->type = fresh(c);
            }

            return sym->type;
        }

        return fresh(c);
    }

    case PARSE_INFER_TVAR:
        return tvar(c, e->name);

    case PARSE_INFER_NAMED:
        return th_name(c->tc, e->name.data ? e->name.data : "");

    case PARSE_INFER_NAMED_APP: {
        core_list_t(th_arg) args = core_list_new(th_arg);

        app_args(c, e, &args);

        th_type *r = th_apply(c->tc, e->name.data ? e->name.data : "",
                              args.data, args.len);

        core_list_free(args);

        return r;
    }

    case PARSE_INFER_CHILD_APP: {
        parse_tree_t *k  = child(c, e->index);
        core_string_t      nm = k ? parse_tree_extract_first_identifier(k)
                                 : (core_string_t){0};

        if (!nm.u8_bytes) {
            c->failed = true;

            return nullptr;
        }

        core_list_t(th_arg) args = core_list_new(th_arg);

        app_args(c, e, &args);

        th_type *r = th_apply(c->tc, nm.data, args.data, args.len);

        core_list_free(args);

        return r;
    }

    case PARSE_INFER_SPREAD:
        c->failed = true;

        return nullptr;

    case PARSE_INFER_LOOKUP: {
        th_type *t = symbol_type(c, e->index);

        return t ? t : fresh(c);
    }

    case PARSE_INFER_PROJECT:
        return project(c, e->proj, e->index);

    case PARSE_INFER_UNIFY: {
        th_type *l = eval(c, e->lhs);
        th_type *r = eval(c, e->rhs);

        if (!l || !r) {
            c->failed = true;

            return l;
        }

        th_unify(c->tc, l, r);

        return l;
    }

    case PARSE_INFER_SUM: {
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
parse_infer_eval(const parse_infer_expr_t *e,
               parse_visit_t            *v,
               th_ctx                 *tc,
               parse_symtab_t           *st,
               bool                   *ok)
{
    if (ok) {
        *ok = false;
    }

    if (!e || !v || !tc) {
        return nullptr;
    }

    parse_tree_t *first_leaf = parse_tree_scan_leaves(v->node, false, nullptr,
                                                       nullptr);
    parse_tree_t *last_leaf  = parse_tree_scan_leaves(v->node, true, nullptr,
                                                       nullptr);
    parse_token_info_t *first = first_leaf ? core_tree_leaf_value(first_leaf)
                                           : nullptr;
    parse_token_info_t *last  = last_leaf ? core_tree_leaf_value(last_leaf)
                                          : first;

    if (first) {
        const char *file = core_option_is_set(first->file)
                               ? core_option_get(first->file).data
                               : nullptr;

        th_ctx_set_span(tc,
                        (th_span_t){.file = file,
                                    .start_line = first->line,
                                    .start_col = first->column,
                                    .end_line = last && last->endline
                                                    ? last->endline
                                                    : last ? last->line
                                                           : first->line,
                                    .end_col = last && last->endcol
                                                   ? last->endcol
                                                   : last ? last->column + 1
                                                          : first->column + 1});
    }

    ictx c = {.tc = tc, .st = st, .v = v, .tvars = core_list_new(tvar_t)};

    th_type *t = eval(&c, e);

    if (ok) {
        *ok = !c.failed && t != nullptr;
    }

    core_list_free(c.tvars);

    return t;
}
