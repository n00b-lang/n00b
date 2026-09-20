#include "parse/infer.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "private/parse/annot_grammar.h"
#include "private/parse/grammar.h"
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
learn_tids(parse_grammar_t *g)
{
    if (tid_unify) {
        return;
    }

#define TID(n) parse_lookup_terminal(g, CORE_STRING_STATIC(n))

    tid_int           = PARSE_TOK_INTEGER;
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
    parse_infer_expr_t *expr;
    parse_infer_expr_t *tail;
    int64_t           tid;
    core_string_t      text;
} in_node;

typedef in_node *in_node_ptr_t;
core_list_decl(in_node_ptr_t);

struct parse_infer_lower_t {
    bool failed;
    core_list_t(in_node_ptr_t) nodes;
};

typedef struct parse_infer_lower_t in_ctx;

static in_node *
node_new(in_ctx *c, in_tag tag)
{
    in_node *n = core_alloc(in_node);

    n->magic = IN_MAGIC;
    n->tag   = tag;

    if (c) {
        core_list_push(c->nodes, n);
    }

    return n;
}

static in_node *
wrap(in_ctx *c, parse_infer_expr_t *e)
{
    in_node *n = node_new(c, IN_EXPR);

    n->expr = e;

    return n;
}

static parse_infer_expr_t *
expr_new(parse_infer_kind_t kind)
{
    parse_infer_expr_t *e = core_alloc(parse_infer_expr_t);

    e->kind  = kind;
    e->index = -1;

    return e;
}

static in_node *
kid(parse_reduce_t *r, size_t i)
{
    return i < r->n_kids ? (in_node *)r->kids[i] : nullptr;
}

static parse_infer_expr_t *
kid_expr(parse_reduce_t *r, size_t i)
{
    in_node *n = kid(r, i);

    return n && n->tag == IN_EXPR ? n->expr : nullptr;
}

static int32_t
kid_index(parse_reduce_t *r, size_t i)
{
    in_node *n = kid(r, i);

    if (!n || n->tag != IN_LEAF || n->tid != tid_int || !n->text.data) {
        return -1;
    }

    return (int32_t)strtol(n->text.data, nullptr, 10);
}

static core_string_t
kid_text(parse_reduce_t *r, size_t i)
{
    in_node *n = kid(r, i);

    return n && n->tag == IN_LEAF ? n->text : (core_string_t){0};
}

static core_string_t
dup_text(core_string_t text)
{
    return text.data ? core_string_dup(text) : text;
}

static void
fail(parse_reduce_t *r)
{
    in_ctx *c = r->thunk;

    if (c) {
        c->failed = true;
    }
}

static void *
r_leaf(parse_reduce_t *r)
{
    in_node *n = node_new(r->thunk, IN_LEAF);

    n->tid  = parse_tree_leaf_tid(r->node);
    n->text = parse_tree_leaf_text(r->node);

    return n;
}

static void *
r_pass(parse_reduce_t *r)
{
    return kid(r, 0);
}

static void *
r_group(parse_reduce_t *r)
{
    return kid(r, 1);
}

static void *
r_op(parse_reduce_t *r)
{
    in_node *n  = node_new(r->thunk, IN_OP);
    in_node *lf = kid(r, 0);

    n->tid = lf ? lf->tid : 0;

    return n;
}

static void *
r_tail(parse_reduce_t *r)
{
    in_node *op   = kid(r, 0);
    in_node *n    = node_new(r->thunk, IN_TAIL);
    parse_infer_expr_t *e = expr_new(op && op->tid == tid_unify
                                       ? PARSE_INFER_UNIFY
                                       : PARSE_INFER_SUM);

    e->rhs  = kid_expr(r, 1);
    n->tail = e;

    if (!e->rhs) {
        fail(r);
    }

    return n;
}

static void *
r_expr(parse_reduce_t *r)
{
    parse_infer_expr_t *lhs = kid_expr(r, 0);

    if (!lhs) {
        fail(r);

        return nullptr;
    }

    parse_reduce_list_t *tails = parse_reduce_as_list(kid(r, 1));

    for (size_t i = 0; tails && i < tails->n; i++) {
        in_node *t = (in_node *)tails->v[i];

        if (!t || t->tag != IN_TAIL || !t->tail) {
            continue;
        }

        t->tail->lhs = lhs;
        lhs          = t->tail;
    }

    return wrap(r->thunk, lhs);
}

static void *
r_self_ref(parse_reduce_t *r)
{
    (void)r;

    return wrap(r->thunk, expr_new(PARSE_INFER_SELF));
}

static void *
r_return_ref(parse_reduce_t *r)
{
    if (!core_string_eq(kid_text(r, 1), CORE_STRING_STATIC("return"))) {
        fail(r);
    }

    return wrap(r->thunk, expr_new(PARSE_INFER_RETURN));
}

static void *
r_child_ref(parse_reduce_t *r)
{
    parse_infer_expr_t *e = expr_new(PARSE_INFER_CHILD);

    e->index = kid_index(r, 1);

    if (e->index < 0) {
        fail(r);
    }

    return wrap(r->thunk, e);
}

static void *
r_type_var(parse_reduce_t *r)
{
    parse_infer_expr_t *e = expr_new(PARSE_INFER_TVAR);

    e->name = dup_text(kid_text(r, 1));

    if (!e->name.data) {
        fail(r);
    }

    return wrap(r->thunk, e);
}

static void *
r_named(parse_reduce_t *r)
{
    parse_infer_expr_t *e = expr_new(PARSE_INFER_NAMED);

    e->name = dup_text(kid_text(r, 0));

    if (!e->name.data) {
        fail(r);
    }

    return wrap(r->thunk, e);
}

static void *
r_spread(parse_reduce_t *r)
{
    parse_infer_expr_t *e = expr_new(PARSE_INFER_SPREAD);

    e->index = kid_index(r, 2);

    if (e->index < 0) {
        fail(r);
    }

    return wrap(r->thunk, e);
}

static size_t
collect(parse_reduce_t *r, parse_infer_expr_t ***out)
{
    parse_infer_expr_t *first = kid_expr(r, 0);
    parse_reduce_list_t *rest = parse_reduce_as_list(kid(r, 1));
    size_t             max  = 1 + (rest ? rest->n : 0);

    parse_infer_expr_t **args = core_alloc_array(parse_infer_expr_t *, max);
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
r_args(parse_reduce_t *r)
{
    parse_infer_expr_t *holder = expr_new(PARSE_INFER_NAMED_APP);

    holder->n_args = collect(r, &holder->args);

    return wrap(r->thunk, holder);
}

static void *
r_named_app(parse_reduce_t *r)
{
    parse_infer_expr_t *e    = expr_new(PARSE_INFER_NAMED_APP);
    parse_infer_expr_t *args = kid_expr(r, 2);

    e->name = dup_text(kid_text(r, 0));

    if (args) {
        e->args   = args->args;
        e->n_args = args->n_args;
        core_free(args);
    }

    if (!e->name.data || !e->n_args) {
        fail(r);
    }

    return wrap(r->thunk, e);
}

static void *
r_child_app(parse_reduce_t *r)
{
    parse_infer_expr_t  *e   = expr_new(PARSE_INFER_CHILD_APP);
    parse_reduce_list_t *opt = parse_reduce_as_list(kid(r, 3));

    e->index = kid_index(r, 1);

    if (e->index < 0) {
        fail(r);
    }

    for (size_t i = 0; opt && i < opt->n; i++) {
        in_node *a = (in_node *)opt->v[i];

        if (a && a->tag == IN_EXPR && a->expr) {
            e->args   = a->expr->args;
            e->n_args = a->expr->n_args;
            core_free(a->expr);
        }
    }

    return wrap(r->thunk, e);
}

static void *
r_fetch(parse_reduce_t *r)
{
    parse_infer_expr_t *e   = expr_new(PARSE_INFER_LOOKUP);
    parse_infer_expr_t *ref = kid_expr(r, 2);

    e->index = ref ? ref->index : -1;

    if (ref) {
        core_free(ref);
    }

    if (e->index < 0) {
        fail(r);
    }

    return wrap(r->thunk, e);
}

static void *
r_project(parse_reduce_t *r)
{
    parse_infer_expr_t *e   = expr_new(PARSE_INFER_PROJECT);
    in_node          *op  = kid(r, 0);
    parse_infer_expr_t *ref = kid_expr(r, 2);
    int64_t           tid = op && op->tag == IN_OP ? op->tid : 0;

    if (tid == tid_return_of) {
        e->proj = PARSE_INFER_RETURN_OF;
    }
    else if (tid == tid_element_of) {
        e->proj = PARSE_INFER_ELEMENT_OF;
    }
    else if (tid == tid_unwrap) {
        e->proj = PARSE_INFER_UNWRAP_RESULT;
    }
    else if (tid == tid_method_return) {
        e->proj = PARSE_INFER_METHOD_RETURN;
    }
    else {
        fail(r);
    }

    e->index = ref ? ref->index : -1;

    if (ref) {
        core_free(ref);
    }

    if (e->index < 0) {
        fail(r);
    }

    return wrap(r->thunk, e);
}

static void
install(parse_tree_walk_t *w, in_ctx *c)
{
    parse_grammar_t *g = parse_annot_grammar();

    parse_walk_reduce_leaf_in(w, parse_nt_unit(g, CORE_STRING_STATIC("infer-expr")),
                            r_leaf, c);

#define REG(name, fn) parse_walk_reduce_nt(w, (name), fn, c)

    REG(CORE_STRING_STATIC("infer-expr"), r_expr);
    REG(CORE_STRING_STATIC("infer-tail"), r_tail);
    REG(CORE_STRING_STATIC("infer-op"), r_op);
    REG(CORE_STRING_STATIC("infer-term"), r_pass);
    REG(CORE_STRING_STATIC("self-ref"), r_self_ref);
    REG(CORE_STRING_STATIC("return-ref"), r_return_ref);
    REG(CORE_STRING_STATIC("child-ref"), r_child_ref);
    REG(CORE_STRING_STATIC("child-app"), r_child_app);
    REG(CORE_STRING_STATIC("child-args"), r_args);
    REG(CORE_STRING_STATIC("child-arg"), r_pass);
    REG(CORE_STRING_STATIC("spread"), r_spread);
    REG(CORE_STRING_STATIC("type-var"), r_type_var);
    REG(CORE_STRING_STATIC("group"), r_group);
    REG(CORE_STRING_STATIC("fetch"), r_fetch);
    REG(CORE_STRING_STATIC("project"), r_project);
    REG(CORE_STRING_STATIC("project-op"), r_op);
    REG(CORE_STRING_STATIC("named-app"), r_named_app);
    REG(CORE_STRING_STATIC("named-args"), r_args);
    REG(CORE_STRING_STATIC("named"), r_named);

#undef REG
}

parse_infer_lower_t *
parse_infer_lower_new(void)
{
    parse_infer_lower_t *l = core_alloc(parse_infer_lower_t);
    l->nodes = core_list_new(in_node_ptr_t);
    return l;
}

void
parse_infer_lower_free(parse_infer_lower_t *l)
{
    if (l) {
        for (size_t i = 0; i < l->nodes.len; i++) {
            core_free(l->nodes.data[i]);
        }
        core_list_free(l->nodes);
        core_free(l);
    }
}

bool
parse_infer_lower_failed(const parse_infer_lower_t *l)
{
    return !l || l->failed;
}

void
parse_infer_install(parse_tree_walk_t *w, parse_infer_lower_t *l)
{
    learn_tids(parse_annot_grammar());
    install(w, l);
}

parse_infer_expr_t *
parse_infer_reduced(void *result)
{
    in_node *n = result;

    if (!n || parse_reduce_as_list(result) || n->magic != IN_MAGIC) {
        return nullptr;
    }

    return n->tag == IN_EXPR ? n->expr : nullptr;
}

parse_infer_expr_t *
parse_infer_parse(core_string_t expr)
{
    parse_grammar_t *g = parse_annot_grammar();

    if (!g || !expr.data) {
        return nullptr;
    }

    core_buffer_t       *buf = nullptr;
    parse_scanner_t      *sc  = nullptr;
    parse_token_stream_t *ts  = parse_annot_lex(expr, &buf, &sc);

    parse_nt_id_t start = parse_nonterm_id(
        parse_nonterm(g, CORE_STRING_STATIC("infer-expr")));
    parse_pwz_parser_t *p   = parse_pwz_new_for_start(g, start);
    parse_infer_expr_t *out = nullptr;

    if (parse_pwz_parse(p, ts)) {
        in_ctx           c = {0};
        parse_tree_walk_t *w = parse_tree_walk_new(g, &c);

        parse_infer_install(w, &c);

        in_node *top = (in_node *)parse_tree_walk_reduce(w,
                                                       parse_pwz_get_tree(p));

        parse_tree_walk_free(w);

        if (!c.failed && top && top->tag == IN_EXPR) {
            out = top->expr;
        }
    }

    parse_pwz_free(p);
    parse_annot_lex_free(ts, buf, sc);

    return out;
}

parse_infer_expr_t *
parse_infer_clone(const parse_infer_expr_t *e)
{
    if (!e) {
        return nullptr;
    }

    parse_infer_expr_t *c = core_alloc(parse_infer_expr_t);

    *c = *e;

    c->name = e->name.data
                ? core_string_from_raw(e->name.data, (int64_t)e->name.u8_bytes)
                : e->name;

    c->lhs = parse_infer_clone(e->lhs);
    c->rhs = parse_infer_clone(e->rhs);

    if (e->n_args) {
        c->args = core_alloc_array(parse_infer_expr_t *, e->n_args);

        for (size_t i = 0; i < e->n_args; i++) {
            c->args[i] = parse_infer_clone(e->args[i]);
        }
    }

    return c;
}

void
parse_infer_free(parse_infer_expr_t *e)
{
    if (!e) {
        return;
    }

    parse_infer_free(e->lhs);
    parse_infer_free(e->rhs);

    for (size_t i = 0; i < e->n_args; i++) {
        parse_infer_free(e->args[i]);
    }

    if (e->args) {
        core_free(e->args);
    }

    core_free(e->name.data);

    core_free(e);
}

const char *
parse_infer_kind_name(parse_infer_kind_t k)
{
    switch (k) {
    case PARSE_INFER_CHILD:     return "child";
    case PARSE_INFER_SELF:      return "self";
    case PARSE_INFER_RETURN:    return "return";
    case PARSE_INFER_TVAR:      return "tvar";
    case PARSE_INFER_NAMED:     return "named";
    case PARSE_INFER_NAMED_APP: return "named-app";
    case PARSE_INFER_CHILD_APP: return "child-app";
    case PARSE_INFER_SPREAD:    return "spread";
    case PARSE_INFER_LOOKUP:    return "lookup";
    case PARSE_INFER_PROJECT:   return "project";
    case PARSE_INFER_UNIFY:     return "unify";
    case PARSE_INFER_SUM:       return "sum";
    }

    return "?";
}

const char *
parse_infer_proj_name(parse_infer_proj_t p)
{
    switch (p) {
    case PARSE_INFER_RETURN_OF:      return "return_of";
    case PARSE_INFER_ELEMENT_OF:     return "element_of";
    case PARSE_INFER_UNWRAP_RESULT:  return "unwrap_result";
    case PARSE_INFER_METHOD_RETURN:  return "method_return";
    }

    return "?";
}

typedef core_buffer_t sbuf;

static void
emit(sbuf *b, const char *s)
{
    core_buffer_puts(b, s);
}

static void
emit_int(sbuf *b, int32_t v)
{
    core_buffer_printf(b, "%d", v);
}

static void
print_args(sbuf *b, const parse_infer_expr_t *e);

static void
print_one(sbuf *b, const parse_infer_expr_t *e)
{
    if (!e) {
        return;
    }

    switch (e->kind) {
    case PARSE_INFER_CHILD:
        emit(b, "$");
        emit_int(b, e->index);
        break;

    case PARSE_INFER_SELF:
        emit(b, "$$");
        break;

    case PARSE_INFER_RETURN:
        emit(b, "$return");
        break;

    case PARSE_INFER_TVAR:
        emit(b, "`");
        emit(b, e->name.data ? e->name.data : "");
        break;

    case PARSE_INFER_NAMED:
        emit(b, e->name.data ? e->name.data : "");
        break;

    case PARSE_INFER_NAMED_APP:
        emit(b, e->name.data ? e->name.data : "");
        print_args(b, e);
        break;

    case PARSE_INFER_CHILD_APP:
        emit(b, "$");
        emit_int(b, e->index);
        print_args(b, e);
        break;

    case PARSE_INFER_SPREAD:
        emit(b, "...$");
        emit_int(b, e->index);
        break;

    case PARSE_INFER_LOOKUP:
        emit(b, "lookup($");
        emit_int(b, e->index);
        emit(b, ")");
        break;

    case PARSE_INFER_PROJECT:
        emit(b, parse_infer_proj_name(e->proj));
        emit(b, "($");
        emit_int(b, e->index);
        emit(b, ")");
        break;

    case PARSE_INFER_UNIFY:
        emit(b, "(");
        print_one(b, e->lhs);
        emit(b, " unify ");
        print_one(b, e->rhs);
        emit(b, ")");
        break;

    case PARSE_INFER_SUM:
        emit(b, "(");
        print_one(b, e->lhs);
        emit(b, " | ");
        print_one(b, e->rhs);
        emit(b, ")");
        break;
    }
}

static void
print_args(sbuf *b, const parse_infer_expr_t *e)
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
parse_infer_print(const parse_infer_expr_t *e)
{
    sbuf *b = core_buffer_empty();

    print_one(b, e);

    return core_buffer_take(b);
}
