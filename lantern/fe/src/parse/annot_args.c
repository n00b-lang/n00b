#include "parse/annot_args.h"
#include "ir/typelang.h"
#include "parse/infer.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "internal/parse/annot_grammar.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AA_MAGIC 0x41413031u
#define AA_MAX   8

typedef enum : uint8_t {
    AA_REF,
    AA_TAG,
    AA_INT,
    AA_INFER,
    AA_TYPE,
    AA_PRED,
    AA_LEAF,
    AA_ARGS,
} aa_tag;

typedef struct aa_node {
    uint32_t          magic;
    aa_tag            tag;
    ncc_child_ref_t   ref;
    ncc_string_t      str;
    int32_t           i;
    ncc_infer_expr_t *infer;
    ncc_parse_tree_t *tree;
    struct aa_node   *items[AA_MAX];
    size_t            n;
} aa_node;

typedef struct {
    bool         failed;
    ncc_string_t region;
} aa_ctx;

static void
leaf_span(ncc_parse_tree_t *n, size_t *lo, size_t *hi)
{
    if (!n) {
        return;
    }

    if (ncc_tree_is_leaf(n)) {
        ncc_token_info_t *t = ncc_parse_node_token(n);

        if (t) {
            if (t->start_byte < *lo) {
                *lo = t->start_byte;
            }

            if (t->end_byte > *hi) {
                *hi = t->end_byte;
            }
        }

        return;
    }

    size_t nc = ncc_tree_num_children(n);

    for (size_t i = 0; i < nc; i++) {
        leaf_span(ncc_tree_child(n, i), lo, hi);
    }
}

static ncc_string_t
dup(ncc_string_t s)
{
    return s.data ? ncc_string_from_raw(s.data, (int64_t)s.u8_bytes) : s;
}


static aa_node *
aa_new(aa_tag tag)
{
    aa_node *n = ncc_alloc(aa_node);

    n->magic = AA_MAGIC;
    n->tag   = tag;

    return n;
}

static aa_node *
as_aa(void *v)
{
    aa_node *n = v;

    return (n && !ncc_reduce_as_list(v) && n->magic == AA_MAGIC) ? n
                                                                 : nullptr;
}

static aa_node *
kid(ncc_reduce_t *r, size_t i)
{
    return i < r->n_kids ? as_aa(r->kids[i]) : nullptr;
}

static void
fail(ncc_reduce_t *r)
{
    aa_ctx *c = r->thunk;

    if (c) {
        c->failed = true;
    }
}

static void *
r_leaf(ncc_reduce_t *r)
{
    aa_node *n = aa_new(AA_LEAF);

    n->i   = (int32_t)ncc_tree_leaf_tid(r->node);
    n->str = ncc_tree_leaf_text(r->node);

    return n;
}

static void *
r_ref(ncc_reduce_t *r)
{
    aa_node *n    = aa_new(AA_REF);
    aa_node *what = kid(r, 1);

    n->ref = NCC_CHILD_NONE;

    if (!what || !what->str.data) {
        fail(r);
    }
    else if (what->i == NCC_TOK_INTEGER) {
        n->ref = NCC_CHILD_IX((int32_t)strtol(what->str.data, nullptr, 10));
    }
    else {
        n->ref = NCC_CHILD_NT(dup(what->str));
    }

    return n;
}

static void *
r_tag(ncc_reduce_t *r)
{
    aa_node *n  = aa_new(AA_TAG);
    aa_node *lf = kid(r, 0);

    n->str = lf ? lf->str : (ncc_string_t){0};

    return n;
}

static void *
r_int(ncc_reduce_t *r)
{
    aa_node *n  = aa_new(AA_INT);
    aa_node *lf = kid(r, 0);

    n->i = lf && lf->str.data ? (int32_t)strtol(lf->str.data, nullptr, 10) : 0;

    return n;
}

static void *
r_infer(ncc_reduce_t *r)
{
    aa_node *n = aa_new(AA_INFER);

    n->infer = ncc_infer_reduced(r->n_kids ? r->kids[0] : nullptr);

    if (!n->infer) {
        fail(r);
    }

    return n;
}

static void *
r_type(ncc_reduce_t *r)
{
    aa_ctx  *c  = r->thunk;
    aa_node *n  = aa_new(AA_TYPE);
    size_t   lo = (size_t)-1;
    size_t   hi = 0;

    leaf_span(r->node, &lo, &hi);

    if (lo < hi && c && c->region.data && hi <= c->region.u8_bytes) {
        n->str = ncc_string_from_raw(c->region.data + lo, (int64_t)(hi - lo));
    }
    else {
        fail(r);
    }

    return n;
}

static void *
r_pred(ncc_reduce_t *r)
{
    aa_ctx  *c  = r->thunk;
    aa_node *n  = aa_new(AA_PRED);
    size_t   lo = (size_t)-1;
    size_t   hi = 0;

    leaf_span(r->node, &lo, &hi);

    if (lo < hi && c && c->region.data && hi <= c->region.u8_bytes) {
        n->str = ncc_string_from_raw(c->region.data + lo, (int64_t)(hi - lo));
    }
    else {
        fail(r);
    }

    return n;
}

static void *
r_span(ncc_reduce_t *r)
{
    aa_ctx  *c  = r->thunk;
    aa_node *n  = aa_new(AA_TAG);
    size_t   lo = (size_t)-1;
    size_t   hi = 0;

    leaf_span(r->node, &lo, &hi);

    if (lo < hi && c && c->region.data && hi <= c->region.u8_bytes) {
        n->str = ncc_string_from_raw(c->region.data + lo, (int64_t)(hi - lo));
    }
    else {
        fail(r);
    }

    return n;
}

static void *
r_val(ncc_reduce_t *r)
{
    aa_node *inner = kid(r, 0);

    if (inner && (inner->tag == AA_REF || inner->tag == AA_INT)) {
        return inner;
    }

    return r_span(r);
}

static void *
r_args(ncc_reduce_t *r)
{
    aa_node *n = aa_new(AA_ARGS);

    for (size_t i = 0; i < r->n_kids; i++) {
        aa_node *k = as_aa(r->kids[i]);

        if (!k || k->tag == AA_LEAF) {
            continue;
        }

        if (n->n == AA_MAX) {
            fail(r);

            break;
        }

        n->items[n->n++] = k;
    }

    return n;
}

static struct {
    ncc_annot_kind_t kind;
    ncc_string_t     nt;
} const SIGS[] = {
    {NCC_ANNOT_SCOPE_OPEN, NCC_STRING_STATIC("a-scope")},
    {NCC_ANNOT_DECLARES,   NCC_STRING_STATIC("a-declares")},
    {NCC_ANNOT_TYPE,       NCC_STRING_STATIC("a-type")},
    {NCC_ANNOT_TYPE_DECL,  NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_ASSIGNS,    NCC_STRING_STATIC("a-assigns")},
    {NCC_ANNOT_BRANCH,     NCC_STRING_STATIC("a-branch")},
    {NCC_ANNOT_LOOP,       NCC_STRING_STATIC("a-pair")},
    {NCC_ANNOT_SWITCH,     NCC_STRING_STATIC("a-pair")},
    {NCC_ANNOT_CALL,       NCC_STRING_STATIC("a-pair")},
    {NCC_ANNOT_FIELD,      NCC_STRING_STATIC("a-pair")},
    {NCC_ANNOT_METHOD,     NCC_STRING_STATIC("a-pair")},
    {NCC_ANNOT_JUMP,       NCC_STRING_STATIC("a-tag")},
    {NCC_ANNOT_TOKENIZER,  NCC_STRING_STATIC("a-tag")},
    {NCC_ANNOT_VISIBILITY, NCC_STRING_STATIC("a-tag")},
    {NCC_ANNOT_OPERATOR,   NCC_STRING_STATIC("a-tag")},
    {NCC_ANNOT_CAPTURE,    NCC_STRING_STATIC("a-capture")},
    {NCC_ANNOT_ADT,        NCC_STRING_STATIC("a-adt")},
    {NCC_ANNOT_LITERAL,    NCC_STRING_STATIC("a-literal")},
    {NCC_ANNOT_INFER,      NCC_STRING_STATIC("a-infer")},
    {NCC_ANNOT_PENALTY,    NCC_STRING_STATIC("a-int")},
    {NCC_ANNOT_DISAMBIG,   NCC_STRING_STATIC("a-disambig")},
    {NCC_ANNOT_NOTRIVIA,   NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_EXPOSES,    NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_RECORD,     NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_INHERITS,   NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_IMPLEMENTS, NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_VARREF,     NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_INDENT,     NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_GROUP,      NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_ALIGN,      NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_CONCAT,     NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_SPACE,      NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_NOSPACE,    NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_NEWLINE,    NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_SOFTLINE,   NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_HARDLINE,   NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_BLANKLINE,  NCC_STRING_STATIC("a-ref")},
    {NCC_ANNOT_MUTABILITY, NCC_STRING_STATIC("a-mutability")},
    {NCC_ANNOT_NAMESPACE,  NCC_STRING_STATIC("a-namespace")},
    {NCC_ANNOT_DIRECTION,  NCC_STRING_STATIC("a-direction")},
    {NCC_ANNOT_CONSTRAIN,  NCC_STRING_STATIC("a-constrain")},
    {NCC_ANNOT_SIZE,       NCC_STRING_STATIC("a-val")},
    {NCC_ANNOT_ALIGNMENT,  NCC_STRING_STATIC("a-val")},
    {NCC_ANNOT_OFFSET,     NCC_STRING_STATIC("a-val")},
    {NCC_ANNOT_WIDTH,      NCC_STRING_STATIC("a-val")},
};

ncc_string_t
ncc_annot_arg_grammar(ncc_annot_kind_t kind)
{
    for (size_t i = 0; i < sizeof(SIGS) / sizeof(SIGS[0]); i++) {
        if (SIGS[i].kind == kind) {
            return SIGS[i].nt;
        }
    }

    return (ncc_string_t){0};
}

static int32_t
unit_of(ncc_grammar_t *g, ncc_string_t name)
{
    ncc_nonterm_t *nt = ncc_find_nonterm(g, name);

    return nt ? nt->unit : 0;
}

static void
install(ncc_tree_walk_t *w, aa_ctx *c)
{
#define REG(name, fn) ncc_walk_reduce_nt(w, (name), fn, c)

    ncc_walk_reduce_leaf_in(w,
                            unit_of(ncc_annot_grammar(),
                                    NCC_STRING_STATIC("a-ref")),
                            r_leaf,
                            c);

    REG(NCC_STRING_STATIC("ref"), r_ref);
    REG(NCC_STRING_STATIC("tag"), r_tag);
    REG(NCC_STRING_STATIC("name"), r_tag);
    REG(NCC_STRING_STATIC("infer"), r_infer);
    REG(NCC_STRING_STATIC("type"), r_type);
    REG(NCC_STRING_STATIC("pred"), r_pred);
    REG(NCC_STRING_STATIC("pred-call"), r_pred);

    REG(NCC_STRING_STATIC("num"), r_int);
    REG(NCC_STRING_STATIC("val"), r_val);
    REG(NCC_STRING_STATIC("con"), r_span);
    REG(NCC_STRING_STATIC("con-list"), r_span);

    for (size_t i = 0; i < sizeof(SIGS) / sizeof(SIGS[0]); i++) {
        REG(SIGS[i].nt, r_args);
    }

#undef REG
}

static ncc_child_ref_t
arg_ref(aa_node *args, size_t i)
{
    aa_node *a = i < args->n ? args->items[i] : nullptr;

    return a && a->tag == AA_REF ? a->ref : NCC_CHILD_NONE;
}

static ncc_string_t
arg_tag(aa_node *args, size_t i)
{
    aa_node *a = i < args->n ? args->items[i] : nullptr;

    return a && (a->tag == AA_TAG || a->tag == AA_PRED) ? dup(a->str)
                                                        : (ncc_string_t){0};
}

static int32_t
arg_int(aa_node *args, size_t i, int32_t dflt)
{
    aa_node *a = i < args->n ? args->items[i] : nullptr;

    return a && a->tag == AA_INT ? a->i : dflt;
}

static bool
arg_is(aa_node *args, size_t i, aa_tag tag)
{
    return i < args->n && args->items[i]->tag == tag;
}

static void
free_args(aa_node *n)
{
    if (!n) {
        return;
    }

    for (size_t i = 0; i < n->n; i++) {
        free_args(n->items[i]);
    }

    if (n->infer) {
        ncc_infer_free(n->infer);
    }

    ncc_free(n);
}

static bool
decode(ncc_annotation_t *a, aa_node *args)
{
    switch (a->kind) {
    case NCC_ANNOT_SCOPE_OPEN:
        a->scope_tag = arg_tag(args, 0);
        a->name_ref  = arg_ref(args, 1);
        break;

    case NCC_ANNOT_DECLARES:
        a->name_ref = arg_ref(args, 0);

        if (args->n == 2 && arg_is(args, 1, AA_TAG)) {
            a->sym_kind = arg_tag(args, 1);
        }
        else if (args->n >= 2) {
            a->type_ref = arg_ref(args, 1);
            a->sym_kind = arg_tag(args, 2);
        }
        break;

    case NCC_ANNOT_TYPE:
        a->name_ref = arg_ref(args, 0);

        if (args->n < 2) {
            a->kind = NCC_ANNOT_TYPE_DECL;
        }
        else {
            a->type_spec = dup(args->items[1]->str);

            th_ctx *probe = th_ctx_new();
            bool    ok    = ir_type_from_text(probe, a->type_spec) != nullptr;

            th_ctx_free(probe);

            if (!ok) {
                return false;
            }
        }
        break;

    case NCC_ANNOT_ASSIGNS:
        a->name_ref  = arg_ref(args, 0);
        a->value_ref = arg_ref(args, 1);
        break;

    case NCC_ANNOT_BRANCH:
        a->name_ref  = arg_ref(args, 0);
        a->type_ref  = arg_ref(args, 1);
        a->value_ref = arg_ref(args, 2);
        break;

    case NCC_ANNOT_SWITCH:
    case NCC_ANNOT_LOOP:
    case NCC_ANNOT_CALL:
    case NCC_ANNOT_FIELD:
    case NCC_ANNOT_METHOD:
        a->name_ref = arg_ref(args, 0);
        a->type_ref = arg_ref(args, 1);
        break;

    case NCC_ANNOT_JUMP:
    case NCC_ANNOT_TOKENIZER:
        a->scope_tag = arg_tag(args, 0);
        break;

    case NCC_ANNOT_VISIBILITY:
        a->visibility_spec = arg_tag(args, 0);
        break;

    case NCC_ANNOT_OPERATOR:
        a->op_kind = arg_tag(args, 0);
        break;

    case NCC_ANNOT_CAPTURE:
        a->scope_tag      = arg_tag(args, 0);
        a->capture_by_tag = args->n > 1
                         && ncc_string_eq(arg_tag(args, 1),
                                          NCC_STRING_STATIC("dynamic"));
        break;

    case NCC_ANNOT_ADT:
        a->adt_kind        = arg_tag(args, 0);
        a->name_ref        = arg_ref(args, 1);
        a->scope_tag       = arg_tag(args, 2);
        a->adt_keyword_ref = arg_ref(args, 3);
        break;

    case NCC_ANNOT_LITERAL:
        a->op_kind  = arg_tag(args, 0);
        a->type_ref = arg_ref(args, 1);
        break;

    case NCC_ANNOT_PENALTY:
        a->penalty_cost = arg_int(args, 0, 1);

        if (a->penalty_cost <= 0) {
            a->penalty_cost = 1;
        }
        break;

    case NCC_ANNOT_DISAMBIG:
        a->disambig_predicate = arg_tag(args, 0);
        a->disambig_cost      = arg_int(args, 1, 1);

        if (a->disambig_cost <= 0) {
            a->disambig_cost = 1;
        }
        break;

    case NCC_ANNOT_NOTRIVIA:
        a->notrivia_ref = arg_ref(args, 0);
        break;

    case NCC_ANNOT_MUTABILITY:
        a->name_ref   = arg_ref(args, 0);
        a->mutability = arg_tag(args, 1);
        break;

    case NCC_ANNOT_NAMESPACE:
        a->ns_name = arg_tag(args, 0);
        break;

    case NCC_ANNOT_DIRECTION:
        a->name_ref = arg_ref(args, 0);
        a->direction = arg_tag(args, 1);
        break;

    case NCC_ANNOT_CONSTRAIN:
        a->name_ref    = arg_ref(args, 0);
        a->constraints = arg_tag(args, 1);
        break;

    case NCC_ANNOT_SIZE:
    case NCC_ANNOT_ALIGNMENT:
    case NCC_ANNOT_OFFSET:
    case NCC_ANNOT_WIDTH:
        if (arg_is(args, 0, AA_REF)) {
            a->val_ref = arg_ref(args, 0);
        }
        else if (arg_is(args, 0, AA_INT)) {
            a->val_int     = args->items[0]->i;
            a->has_val_int = true;
        }
        else {
            a->val_expr = arg_tag(args, 0);
        }
        break;

    case NCC_ANNOT_INFER: {
        size_t at = args->n > 1 ? 1 : 0;

        if (at) {
            a->name_ref = arg_ref(args, 0);
        }

        if (arg_is(args, at, AA_INFER)) {
            a->infer_ast           = args->items[at]->infer;
            args->items[at]->infer = nullptr;
        }
        else {
            a->infer_ast = ncc_infer_parse(args->items[at]->str);
        }

        if (!a->infer_ast) {
            return false;
        }

        char *text = ncc_infer_print(a->infer_ast);

        a->infer_expr = ncc_string_from_raw(text, (int64_t)strlen(text));
        break;
    }

    default:
        a->name_ref = arg_ref(args, 0);
        break;
    }

    return true;
}

bool
ncc_annot_parse_args(ncc_annotation_t *a, ncc_string_t text)
{
    ncc_string_t nt_name = a ? ncc_annot_arg_grammar(a->kind)
                             : (ncc_string_t){0};

    if (!nt_name.data) {
        return false;
    }

    ncc_grammar_t *g = ncc_annot_grammar();

    if (!g) {
        return false;
    }

    ncc_nonterm_t *start = ncc_find_nonterm(g, nt_name);

    if (!start || !ncc_list_len(g->rules)) {
        return false;
    }

    ncc_buffer_t       *buf = nullptr;
    ncc_scanner_t      *sc  = nullptr;
    ncc_token_stream_t *ts  = ncc_annot_lex(text, &buf, &sc);

    if (!ts) {
        return false;
    }

    ncc_grammar_set_start(g, start);

    ncc_pwz_parser_t *p  = ncc_pwz_new(g);
    bool              ok = false;

    if (ncc_pwz_parse(p, ts)) {
        aa_ctx             c  = {.region = text};
        ncc_infer_lower_t *il = ncc_infer_lower_new();
        ncc_tree_walk_t   *w  = ncc_tree_walk_new(g, &c);

        install(w, &c);
        ncc_infer_install(w, il);

        aa_node *args = as_aa(ncc_tree_walk_reduce(w, ncc_pwz_get_tree(p)));

        ncc_tree_walk_free(w);

        if (!c.failed && args && args->tag == AA_ARGS) {
            ok = decode(a, args);

            if (ok) {
                a->args_text = dup(text);
            }
        }

        free_args(args);
        ncc_infer_lower_free(il);
    }

    ncc_pwz_free(p);
    ncc_annot_lex_free(ts, buf, sc);

    return ok;
}
