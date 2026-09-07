#include "parse/infer.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "internal/parse/annot_grammar.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_MAGIC 0x496e4645u

static int64_t tid_int;
static int64_t tid_unify;
static int64_t tid_return_of;
static int64_t tid_element_of;
static int64_t tid_unwrap;
static int64_t tid_method_return;

static void
learn_tids(ncc_grammar_t *g)
{
    if (tid_unify) {
        return;
    }

#define TID(n) ncc_lookup_terminal(g, NCC_STRING_STATIC(n))

    tid_int           = NCC_TOK_INTEGER;
    tid_unify         = TID("unify");
    tid_return_of     = TID("return_of");
    tid_element_of    = TID("element_of");
    tid_unwrap        = TID("unwrap_result");
    tid_method_return = TID("method_return");

#undef TID
}

typedef enum : uint8_t {
    IN_EXPR,
    IN_OP,
    IN_TAIL,
    IN_LEAF,
} in_tag;

typedef struct {
    uint32_t          magic;
    in_tag            tag;
    ncc_infer_expr_t *expr;
    ncc_infer_expr_t *tail;
    int64_t           tid;
    ncc_string_t      text;
} in_node;

struct ncc_infer_lower_t {
    bool failed;
};

typedef struct ncc_infer_lower_t in_ctx;

static in_node *
node_new(in_tag tag)
{
    in_node *n = ncc_alloc(in_node);

    n->magic = IN_MAGIC;
    n->tag   = tag;

    return n;
}

static in_node *
wrap(ncc_infer_expr_t *e)
{
    in_node *n = node_new(IN_EXPR);

    n->expr = e;

    return n;
}

static ncc_infer_expr_t *
expr_new(ncc_infer_kind_t kind)
{
    ncc_infer_expr_t *e = ncc_alloc(ncc_infer_expr_t);

    e->kind  = kind;
    e->index = -1;

    return e;
}

static in_node *
kid(ncc_reduce_t *r, size_t i)
{
    return i < r->n_kids ? (in_node *)r->kids[i] : nullptr;
}

static ncc_infer_expr_t *
kid_expr(ncc_reduce_t *r, size_t i)
{
    in_node *n = kid(r, i);

    return n && n->tag == IN_EXPR ? n->expr : nullptr;
}

static int32_t
kid_index(ncc_reduce_t *r, size_t i)
{
    in_node *n = kid(r, i);

    if (!n || n->tag != IN_LEAF || n->tid != tid_int || !n->text.data) {
        return -1;
    }

    return (int32_t)strtol(n->text.data, nullptr, 10);
}

static ncc_string_t
kid_text(ncc_reduce_t *r, size_t i)
{
    in_node *n = kid(r, i);

    return n && n->tag == IN_LEAF ? n->text : (ncc_string_t){0};
}

static void
fail(ncc_reduce_t *r)
{
    in_ctx *c = r->thunk;

    if (c) {
        c->failed = true;
    }
}

static void *
r_leaf(ncc_reduce_t *r)
{
    in_node *n = node_new(IN_LEAF);

    n->tid  = ncc_tree_leaf_tid(r->node);
    n->text = ncc_tree_leaf_text(r->node);

    return n;
}

static void *
r_pass(ncc_reduce_t *r)
{
    return kid(r, 0);
}

static void *
r_group(ncc_reduce_t *r)
{
    return kid(r, 1);
}

static void *
r_op(ncc_reduce_t *r)
{
    in_node *n  = node_new(IN_OP);
    in_node *lf = kid(r, 0);

    n->tid = lf ? lf->tid : 0;

    return n;
}

static void *
r_tail(ncc_reduce_t *r)
{
    in_node *op   = kid(r, 0);
    in_node *n    = node_new(IN_TAIL);
    ncc_infer_expr_t *e = expr_new(op && op->tid == tid_unify
                                       ? NCC_INFER_UNIFY
                                       : NCC_INFER_SUM);

    e->rhs  = kid_expr(r, 1);
    n->tail = e;

    if (!e->rhs) {
        fail(r);
    }

    return n;
}

static void *
r_expr(ncc_reduce_t *r)
{
    ncc_infer_expr_t *lhs = kid_expr(r, 0);

    if (!lhs) {
        fail(r);

        return nullptr;
    }

    ncc_reduce_list_t *tails = ncc_reduce_as_list(kid(r, 1));

    for (size_t i = 0; tails && i < tails->n; i++) {
        in_node *t = (in_node *)tails->v[i];

        if (!t || t->tag != IN_TAIL || !t->tail) {
            continue;
        }

        t->tail->lhs = lhs;
        lhs          = t->tail;
    }

    return wrap(lhs);
}

static void *
r_self_ref(ncc_reduce_t *r)
{
    (void)r;

    return wrap(expr_new(NCC_INFER_SELF));
}

static void *
r_return_ref(ncc_reduce_t *r)
{
    if (!ncc_string_eq(kid_text(r, 1), NCC_STRING_STATIC("return"))) {
        fail(r);
    }

    return wrap(expr_new(NCC_INFER_RETURN));
}

static void *
r_child_ref(ncc_reduce_t *r)
{
    ncc_infer_expr_t *e = expr_new(NCC_INFER_CHILD);

    e->index = kid_index(r, 1);

    if (e->index < 0) {
        fail(r);
    }

    return wrap(e);
}

static void *
r_type_var(ncc_reduce_t *r)
{
    ncc_infer_expr_t *e = expr_new(NCC_INFER_TVAR);

    e->name = kid_text(r, 1);

    if (!e->name.data) {
        fail(r);
    }

    return wrap(e);
}

static void *
r_named(ncc_reduce_t *r)
{
    ncc_infer_expr_t *e = expr_new(NCC_INFER_NAMED);

    e->name = kid_text(r, 0);

    if (!e->name.data) {
        fail(r);
    }

    return wrap(e);
}

static void *
r_spread(ncc_reduce_t *r)
{
    ncc_infer_expr_t *e = expr_new(NCC_INFER_SPREAD);

    e->index = kid_index(r, 2);

    if (e->index < 0) {
        fail(r);
    }

    return wrap(e);
}

static size_t
collect(ncc_reduce_t *r, ncc_infer_expr_t ***out)
{
    ncc_infer_expr_t *first = kid_expr(r, 0);
    ncc_reduce_list_t *rest = ncc_reduce_as_list(kid(r, 1));
    size_t             max  = 1 + (rest ? rest->n : 0);

    ncc_infer_expr_t **args = ncc_alloc_array(ncc_infer_expr_t *, max);
    size_t             n    = 0;

    if (first) {
        args[n++] = first;
    }

    for (size_t i = 0; rest && i < rest->n; i++) {
        in_node *e = (in_node *)rest->v[i];

        if (e && e->tag == IN_EXPR && e->expr) {
            args[n++] = e->expr;
        }
    }

    *out = args;

    return n;
}

static void *
r_args(ncc_reduce_t *r)
{
    ncc_infer_expr_t *holder = expr_new(NCC_INFER_NAMED_APP);

    holder->n_args = collect(r, &holder->args);

    return wrap(holder);
}

static void *
r_named_app(ncc_reduce_t *r)
{
    ncc_infer_expr_t *e    = expr_new(NCC_INFER_NAMED_APP);
    ncc_infer_expr_t *args = kid_expr(r, 2);

    e->name = kid_text(r, 0);

    if (args) {
        e->args   = args->args;
        e->n_args = args->n_args;
        ncc_free(args);
    }

    if (!e->name.data || !e->n_args) {
        fail(r);
    }

    return wrap(e);
}

static void *
r_child_app(ncc_reduce_t *r)
{
    ncc_infer_expr_t  *e   = expr_new(NCC_INFER_CHILD_APP);
    ncc_reduce_list_t *opt = ncc_reduce_as_list(kid(r, 3));

    e->index = kid_index(r, 1);

    if (e->index < 0) {
        fail(r);
    }

    for (size_t i = 0; opt && i < opt->n; i++) {
        in_node *a = (in_node *)opt->v[i];

        if (a && a->tag == IN_EXPR && a->expr) {
            e->args   = a->expr->args;
            e->n_args = a->expr->n_args;
            ncc_free(a->expr);
        }
    }

    return wrap(e);
}

static void *
r_fetch(ncc_reduce_t *r)
{
    ncc_infer_expr_t *e   = expr_new(NCC_INFER_LOOKUP);
    ncc_infer_expr_t *ref = kid_expr(r, 2);

    e->index = ref ? ref->index : -1;

    if (ref) {
        ncc_free(ref);
    }

    if (e->index < 0) {
        fail(r);
    }

    return wrap(e);
}

static void *
r_project(ncc_reduce_t *r)
{
    ncc_infer_expr_t *e   = expr_new(NCC_INFER_PROJECT);
    in_node          *op  = kid(r, 0);
    ncc_infer_expr_t *ref = kid_expr(r, 2);
    int64_t           tid = op && op->tag == IN_OP ? op->tid : 0;

    if (tid == tid_return_of) {
        e->proj = NCC_INFER_RETURN_OF;
    }
    else if (tid == tid_element_of) {
        e->proj = NCC_INFER_ELEMENT_OF;
    }
    else if (tid == tid_unwrap) {
        e->proj = NCC_INFER_UNWRAP_RESULT;
    }
    else if (tid == tid_method_return) {
        e->proj = NCC_INFER_METHOD_RETURN;
    }
    else {
        fail(r);
    }

    e->index = ref ? ref->index : -1;

    if (ref) {
        ncc_free(ref);
    }

    if (e->index < 0) {
        fail(r);
    }

    return wrap(e);
}

static void
install(ncc_tree_walk_t *w, in_ctx *c)
{
    ncc_grammar_t *g = ncc_annot_grammar();

    ncc_walk_reduce_leaf_in(w, ncc_nt_unit(g, NCC_STRING_STATIC("infer-expr")),
                            r_leaf, c);

#define REG(name, fn) ncc_walk_reduce_nt(w, (name), fn, c)

    REG(NCC_STRING_STATIC("infer-expr"), r_expr);
    REG(NCC_STRING_STATIC("infer-tail"), r_tail);
    REG(NCC_STRING_STATIC("infer-op"), r_op);
    REG(NCC_STRING_STATIC("infer-term"), r_pass);
    REG(NCC_STRING_STATIC("self-ref"), r_self_ref);
    REG(NCC_STRING_STATIC("return-ref"), r_return_ref);
    REG(NCC_STRING_STATIC("child-ref"), r_child_ref);
    REG(NCC_STRING_STATIC("child-app"), r_child_app);
    REG(NCC_STRING_STATIC("child-args"), r_args);
    REG(NCC_STRING_STATIC("child-arg"), r_pass);
    REG(NCC_STRING_STATIC("spread"), r_spread);
    REG(NCC_STRING_STATIC("type-var"), r_type_var);
    REG(NCC_STRING_STATIC("group"), r_group);
    REG(NCC_STRING_STATIC("fetch"), r_fetch);
    REG(NCC_STRING_STATIC("project"), r_project);
    REG(NCC_STRING_STATIC("project-op"), r_op);
    REG(NCC_STRING_STATIC("named-app"), r_named_app);
    REG(NCC_STRING_STATIC("named-args"), r_args);
    REG(NCC_STRING_STATIC("named"), r_named);

#undef REG
}

ncc_infer_lower_t *
ncc_infer_lower_new(void)
{
    return ncc_alloc(ncc_infer_lower_t);
}

void
ncc_infer_lower_free(ncc_infer_lower_t *l)
{
    if (l) {
        ncc_free(l);
    }
}

bool
ncc_infer_lower_failed(const ncc_infer_lower_t *l)
{
    return !l || l->failed;
}

void
ncc_infer_install(ncc_tree_walk_t *w, ncc_infer_lower_t *l)
{
    learn_tids(ncc_annot_grammar());
    install(w, l);
}

ncc_infer_expr_t *
ncc_infer_reduced(void *result)
{
    in_node *n = result;

    if (!n || ncc_reduce_as_list(result) || n->magic != IN_MAGIC) {
        return nullptr;
    }

    return n->tag == IN_EXPR ? n->expr : nullptr;
}

ncc_infer_expr_t *
ncc_infer_parse(ncc_string_t expr)
{
    ncc_grammar_t *g = ncc_annot_grammar();

    if (!g || !expr.data) {
        return nullptr;
    }

    ncc_buffer_t       *buf = nullptr;
    ncc_scanner_t      *sc  = nullptr;
    ncc_token_stream_t *ts  = ncc_annot_lex(expr, &buf, &sc);

    ncc_grammar_set_start(g, ncc_nonterm(g, NCC_STRING_STATIC("infer-expr")));

    ncc_pwz_parser_t *p   = ncc_pwz_new(g);
    ncc_infer_expr_t *out = nullptr;

    if (ncc_pwz_parse(p, ts)) {
        in_ctx           c = {0};
        ncc_tree_walk_t *w = ncc_tree_walk_new(g, &c);

        ncc_infer_install(w, &c);

        in_node *top = (in_node *)ncc_tree_walk_reduce(w,
                                                       ncc_pwz_get_tree(p));

        ncc_tree_walk_free(w);

        if (!c.failed && top && top->tag == IN_EXPR) {
            out = top->expr;
        }
    }

    ncc_pwz_free(p);
    ncc_annot_lex_free(ts, buf, sc);

    return out;
}

ncc_infer_expr_t *
ncc_infer_clone(const ncc_infer_expr_t *e)
{
    if (!e) {
        return nullptr;
    }

    ncc_infer_expr_t *c = ncc_alloc(ncc_infer_expr_t);

    *c = *e;

    c->name = e->name.data
                ? ncc_string_from_raw(e->name.data, (int64_t)e->name.u8_bytes)
                : e->name;

    c->lhs = ncc_infer_clone(e->lhs);
    c->rhs = ncc_infer_clone(e->rhs);

    if (e->n_args) {
        c->args = ncc_alloc_array(ncc_infer_expr_t *, e->n_args);

        for (size_t i = 0; i < e->n_args; i++) {
            c->args[i] = ncc_infer_clone(e->args[i]);
        }
    }

    return c;
}

void
ncc_infer_free(ncc_infer_expr_t *e)
{
    if (!e) {
        return;
    }

    ncc_infer_free(e->lhs);
    ncc_infer_free(e->rhs);

    for (size_t i = 0; i < e->n_args; i++) {
        ncc_infer_free(e->args[i]);
    }

    if (e->args) {
        ncc_free(e->args);
    }

    ncc_free(e);
}

const char *
ncc_infer_kind_name(ncc_infer_kind_t k)
{
    switch (k) {
    case NCC_INFER_CHILD:     return "child";
    case NCC_INFER_SELF:      return "self";
    case NCC_INFER_RETURN:    return "return";
    case NCC_INFER_TVAR:      return "tvar";
    case NCC_INFER_NAMED:     return "named";
    case NCC_INFER_NAMED_APP: return "named-app";
    case NCC_INFER_CHILD_APP: return "child-app";
    case NCC_INFER_SPREAD:    return "spread";
    case NCC_INFER_LOOKUP:    return "lookup";
    case NCC_INFER_PROJECT:   return "project";
    case NCC_INFER_UNIFY:     return "unify";
    case NCC_INFER_SUM:       return "sum";
    }

    return "?";
}

const char *
ncc_infer_proj_name(ncc_infer_proj_t p)
{
    switch (p) {
    case NCC_INFER_RETURN_OF:      return "return_of";
    case NCC_INFER_ELEMENT_OF:     return "element_of";
    case NCC_INFER_UNWRAP_RESULT:  return "unwrap_result";
    case NCC_INFER_METHOD_RETURN:  return "method_return";
    }

    return "?";
}

typedef ncc_buffer_t sbuf;

static void
emit(sbuf *b, const char *s)
{
    ncc_buffer_puts(b, s);
}

static void
emit_int(sbuf *b, int32_t v)
{
    ncc_buffer_printf(b, "%d", v);
}

static void
print_args(sbuf *b, const ncc_infer_expr_t *e);

static void
print_one(sbuf *b, const ncc_infer_expr_t *e)
{
    if (!e) {
        return;
    }

    switch (e->kind) {
    case NCC_INFER_CHILD:
        emit(b, "$");
        emit_int(b, e->index);
        break;

    case NCC_INFER_SELF:
        emit(b, "$$");
        break;

    case NCC_INFER_RETURN:
        emit(b, "$return");
        break;

    case NCC_INFER_TVAR:
        emit(b, "`");
        emit(b, e->name.data ? e->name.data : "");
        break;

    case NCC_INFER_NAMED:
        emit(b, e->name.data ? e->name.data : "");
        break;

    case NCC_INFER_NAMED_APP:
        emit(b, e->name.data ? e->name.data : "");
        print_args(b, e);
        break;

    case NCC_INFER_CHILD_APP:
        emit(b, "$");
        emit_int(b, e->index);
        print_args(b, e);
        break;

    case NCC_INFER_SPREAD:
        emit(b, "...$");
        emit_int(b, e->index);
        break;

    case NCC_INFER_LOOKUP:
        emit(b, "lookup($");
        emit_int(b, e->index);
        emit(b, ")");
        break;

    case NCC_INFER_PROJECT:
        emit(b, ncc_infer_proj_name(e->proj));
        emit(b, "($");
        emit_int(b, e->index);
        emit(b, ")");
        break;

    case NCC_INFER_UNIFY:
        emit(b, "(");
        print_one(b, e->lhs);
        emit(b, " unify ");
        print_one(b, e->rhs);
        emit(b, ")");
        break;

    case NCC_INFER_SUM:
        emit(b, "(");
        print_one(b, e->lhs);
        emit(b, " | ");
        print_one(b, e->rhs);
        emit(b, ")");
        break;
    }
}

static void
print_args(sbuf *b, const ncc_infer_expr_t *e)
{
    emit(b, "[");

    for (size_t i = 0; i < e->n_args; i++) {
        if (i) {
            emit(b, ", ");
        }

        print_one(b, e->args[i]);
    }

    emit(b, "]");
}

char *
ncc_infer_print(const ncc_infer_expr_t *e)
{
    sbuf *b = ncc_buffer_empty();

    print_one(b, e);

    return ncc_buffer_take(b);
}
