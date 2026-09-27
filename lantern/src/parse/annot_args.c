#include "parse/annot_args.h"
#include "ir/typelang.h"
#include "parse/infer.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "private/parse/annot_grammar.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AA_MAGIC 0x41413031u

typedef enum : uint8_t {
    AA_REF,
    AA_TAG,
    AA_INT,
    AA_INFER,
    AA_TYPE,
    AA_PRED,
    AA_LEAF,
    AA_ARGS,
    AA_CON,
    AA_LEXOPT,
} aa_tag;

typedef struct aa_node *aa_node_ptr_t;
core_list_decl(aa_node_ptr_t);

typedef struct aa_node {
    uint32_t          magic;
    aa_tag            tag;
    parse_child_ref_t   ref;
    core_string_t      str;
    int32_t           i;
    uint64_t          index;
    parse_infer_expr_t *infer;
    parse_tree_t *tree;
    core_list_t(aa_node_ptr_t) items;
    size_t            n;
    bool              owns_str;
} aa_node;

typedef struct {
    bool                     failed;
    core_string_t            region;
    core_list_t(aa_node_ptr_t) nodes;
} aa_ctx;

static void
leaf_span(parse_tree_t *n, size_t *lo, size_t *hi)
{
    if (!n) {
        return;
    }

    if (core_tree_is_leaf(n)) {
        parse_token_info_t *t = parse_node_token(n);

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

    size_t nc = core_tree_num_children(n);

    for (size_t i = 0; i < nc; i++) {
        leaf_span(core_tree_child(n, i), lo, hi);
    }
}

static core_string_t
dup(core_string_t s)
{
    return s.data ? core_string_from_raw(s.data, (int64_t)s.u8_bytes) : s;
}


static aa_node *
aa_new(parse_reduce_t *r, aa_tag tag)
{
    aa_node *n = core_alloc(aa_node);

    n->magic = AA_MAGIC;
    n->tag   = tag;

    aa_ctx *c = r ? r->thunk : nullptr;
    if (c) {
        core_list_push(c->nodes, n);
    }

    return n;
}

static void
aa_append(aa_node *n, aa_node *item)
{
    if (!n->items.data) {
        n->items = core_list_new(aa_node_ptr_t);
    }
    core_list_push(n->items, item);
    n->n++;
}

static aa_node *
as_aa(void *v)
{
    aa_node *n = v;

    return (n && !parse_reduce_as_list(v) && n->magic == AA_MAGIC) ? n
                                                                 : nullptr;
}

static aa_node *
kid(parse_reduce_t *r, size_t i)
{
    return i < r->n_kids ? as_aa(r->kids[i]) : nullptr;
}

static void
fail(parse_reduce_t *r)
{
    aa_ctx *c = r->thunk;

    if (c) {
        c->failed = true;
    }
}

static void *
r_leaf(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_LEAF);

    n->i   = (int32_t)parse_tree_leaf_tid(r->node);
    n->str = parse_tree_leaf_text(r->node);

    return n;
}

static void *
r_ref(parse_reduce_t *r)
{
    aa_node *n    = aa_new(r, AA_REF);
    aa_node *what = kid(r, 1);

    n->ref = PARSE_CHILD_NONE;

    if (!what || !what->str.data) {
        fail(r);
    }
    else if (what->i == PARSE_TOK_INTEGER) {
        n->ref = PARSE_CHILD_IX((int32_t)strtol(what->str.data, nullptr, 10));
    }
    else {
        n->ref = PARSE_CHILD_NT(dup(what->str));
    }

    return n;
}

static void *
r_tag(parse_reduce_t *r)
{
    aa_node *n  = aa_new(r, AA_TAG);
    aa_node *lf = kid(r, 0);

    n->str = lf ? lf->str : (core_string_t){0};

    return n;
}

static void *
r_int(parse_reduce_t *r)
{
    aa_node *n  = aa_new(r, AA_INT);
    aa_node *lf = kid(r, 0);

    n->i = lf && lf->str.data ? (int32_t)strtol(lf->str.data, nullptr, 10) : 0;

    return n;
}

static void *
r_infer(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_INFER);

    n->infer = parse_infer_reduced(r->n_kids ? r->kids[0] : nullptr);

    if (!n->infer) {
        fail(r);
    }

    return n;
}

static void *
r_type(parse_reduce_t *r)
{
    aa_ctx  *c  = r->thunk;
    aa_node *n  = aa_new(r, AA_TYPE);
    size_t   lo = (size_t)-1;
    size_t   hi = 0;

    leaf_span(r->node, &lo, &hi);

    if (lo < hi && c && c->region.data && hi <= c->region.u8_bytes) {
        n->str = core_string_from_raw(c->region.data + lo, (int64_t)(hi - lo));
        n->owns_str = true;
    }
    else {
        fail(r);
    }

    return n;
}

static void *
r_pred(parse_reduce_t *r)
{
    aa_ctx  *c  = r->thunk;
    aa_node *n  = aa_new(r, AA_PRED);
    size_t   lo = (size_t)-1;
    size_t   hi = 0;

    leaf_span(r->node, &lo, &hi);

    if (lo < hi && c && c->region.data && hi <= c->region.u8_bytes) {
        n->str = core_string_from_raw(c->region.data + lo, (int64_t)(hi - lo));
        n->owns_str = true;
    }
    else {
        fail(r);
    }

    return n;
}

static void *
r_span(parse_reduce_t *r)
{
    aa_ctx  *c  = r->thunk;
    aa_node *n  = aa_new(r, AA_TAG);
    size_t   lo = (size_t)-1;
    size_t   hi = 0;

    leaf_span(r->node, &lo, &hi);

    if (lo < hi && c && c->region.data && hi <= c->region.u8_bytes) {
        n->str = core_string_from_raw(c->region.data + lo, (int64_t)(hi - lo));
        n->owns_str = true;
    }
    else {
        fail(r);
    }

    return n;
}

static void *
r_val(parse_reduce_t *r)
{
    aa_node *inner = kid(r, 0);

    if (inner && (inner->tag == AA_REF || inner->tag == AA_INT)) {
        return inner;
    }

    return r_span(r);
}

static th_con_kind
con_kind_of(core_string_t op, bool *ok)
{
    static const struct {
        core_string_t op;
        th_con_kind  kind;
    } map[] = {
        {CORE_STRING_STATIC("=="),          TH_CON_UNIFIES   },
        {CORE_STRING_STATIC("!="),          TH_CON_EXCLUDES  },
        {CORE_STRING_STATIC("one_of"),      TH_CON_ONE_OF    },
        {CORE_STRING_STATIC("promotes_to"), TH_CON_PROMOTES  },
        {CORE_STRING_STATIC("has_field"),   TH_CON_HAS_FIELD },
        {CORE_STRING_STATIC("has_param"),   TH_CON_HAS_PARAM },
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (core_string_eq(map[i].op, op)) {
            *ok = true;

            return map[i].kind;
        }
    }

    *ok = false;

    return TH_CON_IMPLEMENTS;
}

static void *
r_con(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_CON);
    bool     named = false;

    for (size_t i = 0; i < r->n_kids; i++) {
        aa_node *k = as_aa(r->kids[i]);

        if (!k) {
            continue;
        }

        if (k->tag == AA_TYPE) {
            aa_append(n, k);
            continue;
        }

        if (k->tag != AA_LEAF) {
            continue;
        }

        if (!named) {
            bool ok = false;

            n->i = (int32_t)con_kind_of(k->str, &ok);

            if (!ok) {
                n->i   = (int32_t)TH_CON_IMPLEMENTS;
                n->str = dup(k->str);
                n->owns_str = true;
            }

            named = true;
            continue;
        }

        if (n->i == (int32_t)TH_CON_HAS_FIELD && !n->str.data) {
            n->str = dup(k->str);
            n->owns_str = true;
        }
        else if (n->i == (int32_t)TH_CON_HAS_PARAM && k->str.data) {
            n->index = (uint64_t)strtoull(k->str.data, nullptr, 10);
        }
    }

    return n;
}

static void *
r_con_list(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_ARGS);

    for (size_t i = 0; i < r->n_kids; i++) {
        aa_node *k = as_aa(r->kids[i]);

        if (!k) {
            continue;
        }

        if (k->tag == AA_CON) {
            aa_append(n, k);
        }
        else if (k->tag == AA_ARGS) {
            for (size_t j = 0; j < k->n; j++) {
                aa_append(n, k->items.data[j]);
            }
        }
    }

    return n;
}

static void *
r_lex_opt(parse_reduce_t *r)
{
    for (size_t i = 0; i < r->n_kids; i++) {
        aa_node *k = as_aa(r->kids[i]);

        if (k && k->tag != AA_LEAF) {
            return k;
        }
    }

    fail(r);

    return aa_new(r, AA_LEXOPT);
}

static void *
r_lex_flag(parse_reduce_t *r)
{
    aa_node *n  = aa_new(r, AA_LEXOPT);
    aa_node *lf = kid(r, 0);

    n->str = lf ? dup(lf->str) : (core_string_t){0};
    n->owns_str = n->str.data != nullptr;

    return n;
}

static bool
aa_push(parse_reduce_t *r, aa_node *n, aa_node *item)
{
    (void)r;
    if (!n->items.data) {
        n->items = core_list_new(aa_node_ptr_t);
    }
    core_list_push(n->items, item);
    n->n++;

    return true;
}

static void
aa_splice(parse_reduce_t *r, aa_node *n, aa_node *from)
{
    for (size_t j = 0; j < from->n; j++) {
        if (!aa_push(r, n, from->items.data[j])) {
            return;
        }
    }
}

static void *
r_lex_call(parse_reduce_t *r)
{
    aa_node *n  = aa_new(r, AA_LEXOPT);
    aa_node *lf = kid(r, 0);

    n->str = lf ? dup(lf->str) : (core_string_t){0};
    n->owns_str = n->str.data != nullptr;

    for (size_t i = 1; i < r->n_kids; i++) {
        aa_node *k = as_aa(r->kids[i]);

        if (!k) {
            continue;
        }

        if (k->tag == AA_ARGS) {
            aa_splice(r, n, k);
        }
        else if (k->tag == AA_TAG) {
            aa_push(r, n, k);
        }
    }

    return n;
}

static void *
r_lex_args(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_ARGS);

    for (size_t i = 0; i < r->n_kids; i++) {
        aa_node *k = as_aa(r->kids[i]);

        if (!k) {
            continue;
        }

        if (k->tag == AA_TAG) {
            aa_push(r, n, k);
        }
        else if (k->tag == AA_ARGS) {
            aa_splice(r, n, k);
        }
    }

    return n;
}

static void *
r_lex_opts(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_ARGS);

    for (size_t i = 0; i < r->n_kids; i++) {
        aa_node *k = as_aa(r->kids[i]);

        if (!k) {
            continue;
        }

        if (k->tag == AA_TAG || k->tag == AA_LEXOPT) {
            aa_push(r, n, k);
        }
        else if (k->tag == AA_ARGS) {
            aa_splice(r, n, k);
        }
    }

    return n;
}

static void *
r_args(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_ARGS);

    for (size_t i = 0; i < r->n_kids; i++) {
        aa_node *k = as_aa(r->kids[i]);

        if (!k || k->tag == AA_LEAF) {
            continue;
        }

        if (!n->items.data) {
            n->items = core_list_new(aa_node_ptr_t);
        }
        core_list_push(n->items, k);
        n->n++;
    }

    return n;
}

static void
emit_args_append(parse_reduce_list_t *list, aa_node *args)
{
    if (!list || !args) return;
    for (size_t i = 0; i < list->n; i++) {
        aa_node *item = as_aa(list->v[i]);
        if (item && item->tag != AA_LEAF) aa_append(args, item);
    }
}

static void *
r_emit_args(parse_reduce_t *r)
{
    aa_node *args = aa_new(r, AA_ARGS);
    for (size_t i = 0; i < r->n_kids; i++) {
        aa_node *item = as_aa(r->kids[i]);
        if (item && item->tag != AA_LEAF) aa_append(args, item);
        else emit_args_append(parse_reduce_as_list(r->kids[i]), args);
    }
    return args;
}

static struct {
    parse_annot_kind_t kind;
    core_string_t     nt;
} const SIGS[] = {
    {PARSE_ANNOT_SCOPE_OPEN, CORE_STRING_STATIC("a-scope")},
    {PARSE_ANNOT_DECLARES,   CORE_STRING_STATIC("a-declares")},
    {PARSE_ANNOT_TYPE,       CORE_STRING_STATIC("a-type")},
    {PARSE_ANNOT_TYPE_DECL,  CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_ASSIGNS,    CORE_STRING_STATIC("a-assigns")},
    {PARSE_ANNOT_BRANCH,     CORE_STRING_STATIC("a-branch")},
    {PARSE_ANNOT_LOOP,       CORE_STRING_STATIC("a-pair")},
    {PARSE_ANNOT_SWITCH,     CORE_STRING_STATIC("a-pair")},
    {PARSE_ANNOT_CALL,       CORE_STRING_STATIC("a-pair")},
    {PARSE_ANNOT_FIELD,      CORE_STRING_STATIC("a-pair")},
    {PARSE_ANNOT_METHOD,     CORE_STRING_STATIC("a-pair")},
    {PARSE_ANNOT_JUMP,       CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_TOKENIZER,  CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_VISIBILITY, CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_OPERATOR,   CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_CAPTURE,    CORE_STRING_STATIC("a-capture")},
    {PARSE_ANNOT_ADT,        CORE_STRING_STATIC("a-adt")},
    {PARSE_ANNOT_LITERAL,    CORE_STRING_STATIC("a-literal")},
    {PARSE_ANNOT_INFER,      CORE_STRING_STATIC("a-infer")},
    {PARSE_ANNOT_PENALTY,    CORE_STRING_STATIC("a-int")},
    {PARSE_ANNOT_DISAMBIG,   CORE_STRING_STATIC("a-disambig")},
    {PARSE_ANNOT_NOTRIVIA,   CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_EXPOSES,    CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_RECORD,     CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_INHERITS,   CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_IMPLEMENTS, CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_VARREF,     CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_INDENT,     CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_GROUP,      CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_ALIGN,      CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_CONCAT,     CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_SPACE,      CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_NOSPACE,    CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_ADJACENT,   CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_NEWLINE,    CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_SOFTLINE,   CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_HARDLINE,   CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_BLANKLINE,  CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_MUTABILITY, CORE_STRING_STATIC("a-mutability")},
    {PARSE_ANNOT_NAMESPACE,  CORE_STRING_STATIC("a-namespace")},
    {PARSE_ANNOT_DIRECTION,  CORE_STRING_STATIC("a-direction")},
    {PARSE_ANNOT_PASS_BY,    CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_CONSTRAIN,  CORE_STRING_STATIC("a-constrain")},
    {PARSE_ANNOT_EMIT,       CORE_STRING_STATIC("a-emit")},
    {PARSE_ANNOT_SIZE,       CORE_STRING_STATIC("a-val")},
    {PARSE_ANNOT_ALIGNMENT,  CORE_STRING_STATIC("a-val")},
    {PARSE_ANNOT_OFFSET,     CORE_STRING_STATIC("a-val")},
    {PARSE_ANNOT_WIDTH,      CORE_STRING_STATIC("a-val")},
    {PARSE_ANNOT_LEXER,      CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_LEX_COMMENT, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_STRING,  CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_STRING_FAMILY, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_STRING_ESCAPES, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_NUMBER,  CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_NUMBER_SUFFIXES, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_LAYOUT,  CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_SIGIL,   CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_REGEX,   CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_HEREDOC_HEADER, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_HEREDOC, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_IDENT,   CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_IDENT_PROFILE, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_EMBED,   CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_FENCE,   CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_USE,     CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_LEX_OPERATOR_SET, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_OPERATORS, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_WHITESPACE, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_FALLBACK, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_DIRECTIVE, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_LITERAL_HOOK, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_RECOVER, CORE_STRING_STATIC("a-lex")},
    {PARSE_ANNOT_LEX_SOFT_KEYWORDS, CORE_STRING_STATIC("a-lex")},
};

core_string_t
parse_annot_arg_grammar(parse_annot_kind_t kind)
{
    for (size_t i = 0; i < sizeof(SIGS) / sizeof(SIGS[0]); i++) {
        if (SIGS[i].kind == kind) {
            return SIGS[i].nt;
        }
    }

    return (core_string_t){0};
}

static void
install(parse_tree_walk_t *w, aa_ctx *c)
{
#define REG(name, fn) parse_walk_reduce_nt(w, (name), fn, c)

    parse_walk_reduce_leaf_in(w,
                            parse_nt_unit(parse_annot_grammar(),
                                    CORE_STRING_STATIC("a-ref")),
                            r_leaf,
                            c);

    REG(CORE_STRING_STATIC("ref"), r_ref);
    REG(CORE_STRING_STATIC("tag"), r_tag);
    REG(CORE_STRING_STATIC("name"), r_tag);
    REG(CORE_STRING_STATIC("infer"), r_infer);
    REG(CORE_STRING_STATIC("type"), r_type);
    REG(CORE_STRING_STATIC("pred"), r_pred);
    REG(CORE_STRING_STATIC("pred-call"), r_pred);

    REG(CORE_STRING_STATIC("num"), r_int);
    REG(CORE_STRING_STATIC("val"), r_val);
    REG(CORE_STRING_STATIC("con"), r_con);
    REG(CORE_STRING_STATIC("con-list"), r_con_list);

    REG(CORE_STRING_STATIC("lex-opt"), r_lex_opt);
    REG(CORE_STRING_STATIC("lex-opts"), r_lex_opts);
    REG(CORE_STRING_STATIC("lex-flag"), r_lex_flag);
    REG(CORE_STRING_STATIC("lex-call"), r_lex_call);
    REG(CORE_STRING_STATIC("lex-args"), r_lex_args);

    for (size_t i = 0; i < sizeof(SIGS) / sizeof(SIGS[0]); i++) {
        REG(SIGS[i].nt, r_args);
    }
    REG(CORE_STRING_STATIC("a-emit"), r_emit_args);

#undef REG
}

static parse_child_ref_t
arg_ref(aa_node *args, size_t i)
{
    aa_node *a = i < args->n ? args->items.data[i] : nullptr;

    if (!a || a->tag != AA_REF) {
        return PARSE_CHILD_NONE;
    }
    parse_child_ref_t result = a->ref;
    result.name = dup(a->ref.name);
    return result;
}

static core_string_t
arg_tag(aa_node *args, size_t i)
{
    aa_node *a = i < args->n ? args->items.data[i] : nullptr;

    return a && (a->tag == AA_TAG || a->tag == AA_PRED) ? dup(a->str)
                                                        : (core_string_t){0};
}

static int32_t
arg_int(aa_node *args, size_t i, int32_t dflt)
{
    aa_node *a = i < args->n ? args->items.data[i] : nullptr;

    return a && a->tag == AA_INT ? a->i : dflt;
}

static bool
arg_is(aa_node *args, size_t i, aa_tag tag)
{
    return i < args->n && args->items.data[i]->tag == tag;
}

static void
free_args(aa_ctx *c)
{
    if (!c) {
        return;
    }

    for (size_t i = 0; i < c->nodes.len; i++) {
        aa_node *n = c->nodes.data[i];
        parse_infer_free(n->infer);
        if (n->owns_str) {
            core_free(n->str.data);
        }
        if (n->tag == AA_REF) {
            core_free(n->ref.name.data);
        }
        core_free(n);
    }
    core_list_free(c->nodes);
}

static void
decode_cons(parse_annotation_t *a, aa_node *list)
{
    if (!list || list->tag != AA_ARGS) {
        return;
    }

    a->constraints = core_list_new(parse_con_spec_t);

    for (size_t i = 0; i < list->n; i++) {
        aa_node *c = list->items.data[i];

        if (!c || c->tag != AA_CON) {
            continue;
        }

        parse_con_spec_t s = {
            .kind  = (th_con_kind)c->i,
            .name  = dup(c->str),
            .types = core_list_new(core_string_t),
            .index = c->index,
        };

        for (size_t j = 0; j < c->n; j++) {
            if (c->items.data[j] && c->items.data[j]->tag == AA_TYPE) {
                core_list_push(s.types, dup(c->items.data[j]->str));
            }
        }

        core_list_push(a->constraints, s);
    }
}

static void
decode_lex(parse_annotation_t *a, aa_node *list)
{
    if (!list || list->tag != AA_ARGS) {
        return;
    }

    a->lex_opts = core_list_new(parse_lex_opt_t);

    for (size_t i = 0; i < list->n; i++) {
        aa_node *o = list->items.data[i];

        if (!o) {
            continue;
        }

        parse_lex_opt_t opt = {.args = core_list_new(core_string_t)};

        if (o->tag == AA_TAG) {
            core_list_push(opt.args, dup(o->str));
        }
        else if (o->tag == AA_LEXOPT) {
            opt.name = dup(o->str);

            for (size_t j = 0; j < o->n; j++) {
                if (o->items.data[j] && o->items.data[j]->tag == AA_TAG) {
                    core_list_push(opt.args, dup(o->items.data[j]->str));
                }
            }
        }
        else {
            core_list_free(opt.args);

            continue;
        }

        core_list_push(a->lex_opts, opt);
    }
}

static bool
decode(parse_annotation_t *a, aa_node *args)
{
    switch (a->kind) {
    case PARSE_ANNOT_SCOPE_OPEN:
        a->scope_tag = arg_tag(args, 0);
        a->name_ref  = arg_ref(args, 1);
        break;

    case PARSE_ANNOT_DECLARES:
        a->name_ref = arg_ref(args, 0);

        if (args->n == 2 && arg_is(args, 1, AA_TAG)) {
            a->sym_kind = arg_tag(args, 1);
        }
        else if (args->n >= 2) {
            a->type_ref = arg_ref(args, 1);
            a->sym_kind = arg_tag(args, 2);
        }
        break;

    case PARSE_ANNOT_TYPE:
        a->name_ref = arg_ref(args, 0);

        if (args->n < 2) {
            a->kind = PARSE_ANNOT_TYPE_DECL;
        }
        else {
            a->type_spec = dup(args->items.data[1]->str);

            th_ctx *probe = th_ctx_new();
            bool    ok    = ir_type_from_text(probe, a->type_spec) != nullptr;

            th_ctx_free(probe);

            if (!ok) {
                return false;
            }
        }
        break;

    case PARSE_ANNOT_ASSIGNS:
        a->name_ref  = arg_ref(args, 0);
        a->value_ref = arg_ref(args, 1);
        break;

    case PARSE_ANNOT_BRANCH:
        a->name_ref  = arg_ref(args, 0);
        a->type_ref  = arg_ref(args, 1);
        a->value_ref = arg_ref(args, 2);
        break;

    case PARSE_ANNOT_SWITCH:
    case PARSE_ANNOT_LOOP:
    case PARSE_ANNOT_CALL:
    case PARSE_ANNOT_FIELD:
    case PARSE_ANNOT_METHOD:
        a->name_ref = arg_ref(args, 0);
        a->type_ref = arg_ref(args, 1);
        break;

    case PARSE_ANNOT_JUMP:
    case PARSE_ANNOT_TOKENIZER:
    case PARSE_ANNOT_LEXER:
    case PARSE_ANNOT_LEX_USE:
        a->scope_tag = arg_tag(args, 0);
        break;

    case PARSE_ANNOT_VISIBILITY:
        a->visibility_spec = arg_tag(args, 0);
        break;

    case PARSE_ANNOT_OPERATOR:
        a->op_kind = arg_tag(args, 0);
        break;

    case PARSE_ANNOT_CAPTURE:
        a->scope_tag      = arg_tag(args, 0);
        a->capture_by_tag = args->n > 1
                         && core_string_eq(arg_tag(args, 1),
                                          CORE_STRING_STATIC("dynamic"));
        break;

    case PARSE_ANNOT_ADT:
        a->adt_kind        = arg_tag(args, 0);
        a->name_ref        = arg_ref(args, 1);
        a->scope_tag       = arg_tag(args, 2);
        a->adt_keyword_ref = arg_ref(args, 3);
        break;

    case PARSE_ANNOT_LITERAL:
        a->op_kind  = arg_tag(args, 0);
        a->type_ref = arg_ref(args, 1);
        break;

    case PARSE_ANNOT_PENALTY:
        a->penalty_cost = arg_int(args, 0, 1);

        if (a->penalty_cost <= 0) {
            a->penalty_cost = 1;
        }
        break;

    case PARSE_ANNOT_DISAMBIG:
        a->disambig_predicate = arg_tag(args, 0);
        a->disambig_cost      = arg_int(args, 1, 1);

        if (a->disambig_cost <= 0) {
            a->disambig_cost = 1;
        }
        break;

    case PARSE_ANNOT_NOTRIVIA:
        a->notrivia_ref = arg_ref(args, 0);
        break;

    case PARSE_ANNOT_ADJACENT:
        a->scope_tag = arg_tag(args, 0);
        if (!core_string_eq(a->scope_tag, CORE_STRING_STATIC("prefix"))) {
            return false;
        }
        break;

    case PARSE_ANNOT_MUTABILITY:
        a->name_ref   = arg_ref(args, 0);
        a->mutability = arg_tag(args, 1);
        break;

    case PARSE_ANNOT_NAMESPACE:
        a->ns_name = arg_tag(args, 0);
        break;

    case PARSE_ANNOT_DIRECTION:
        a->name_ref = arg_ref(args, 0);
        a->direction = arg_tag(args, 1);
        break;

    case PARSE_ANNOT_PASS_BY:
        a->pass_by = arg_tag(args, 0);
        break;

    case PARSE_ANNOT_CONSTRAIN:
        a->name_ref = arg_ref(args, 0);
        decode_cons(a, args->n > 1 ? args->items.data[1] : nullptr);
        break;

    case PARSE_ANNOT_EMIT:
        if (!args->n || !arg_is(args, 0, AA_TAG)) return false;
        a->emit_action = arg_tag(args, 0);
        if (!a->emit_action.data || !a->emit_action.u8_bytes) return false;
        if (args->n > 1) {
            a->emit_operands = core_list_new(parse_child_ref_t);
            for (size_t i = 1; i < args->n; i++) {
                parse_child_ref_t ref = arg_ref(args, i);
                if (!parse_child_ref_is_set(ref)) {
                    core_free(ref.name.data);
                    return false;
                }
                core_list_push(a->emit_operands, ref);
            }
        }
        break;

    case PARSE_ANNOT_LEX_COMMENT:
    case PARSE_ANNOT_LEX_STRING:
    case PARSE_ANNOT_LEX_STRING_FAMILY:
    case PARSE_ANNOT_LEX_STRING_ESCAPES:
    case PARSE_ANNOT_LEX_NUMBER:
    case PARSE_ANNOT_LEX_NUMBER_SUFFIXES:
    case PARSE_ANNOT_LEX_LAYOUT:
    case PARSE_ANNOT_LEX_SIGIL:
    case PARSE_ANNOT_LEX_REGEX:
    case PARSE_ANNOT_LEX_HEREDOC_HEADER:
    case PARSE_ANNOT_LEX_HEREDOC:
    case PARSE_ANNOT_LEX_IDENT:
    case PARSE_ANNOT_LEX_IDENT_PROFILE:
    case PARSE_ANNOT_LEX_EMBED:
    case PARSE_ANNOT_LEX_FENCE:
    case PARSE_ANNOT_LEX_OPERATOR_SET:
    case PARSE_ANNOT_LEX_OPERATORS:
    case PARSE_ANNOT_LEX_WHITESPACE:
    case PARSE_ANNOT_LEX_FALLBACK:
    case PARSE_ANNOT_LEX_DIRECTIVE:
    case PARSE_ANNOT_LEX_LITERAL_HOOK:
    case PARSE_ANNOT_LEX_RECOVER:
    case PARSE_ANNOT_LEX_SOFT_KEYWORDS:
        decode_lex(a, args && args->n ? args->items.data[0] : nullptr);
        break;

    case PARSE_ANNOT_SIZE:
    case PARSE_ANNOT_ALIGNMENT:
    case PARSE_ANNOT_OFFSET:
    case PARSE_ANNOT_WIDTH:
        if (arg_is(args, 0, AA_REF)) {
            a->val_ref = arg_ref(args, 0);
        }
        else if (arg_is(args, 0, AA_INT)) {
            a->val_int     = args->items.data[0]->i;
            a->has_val_int = true;
        }
        else {
            a->val_expr = arg_tag(args, 0);
        }
        break;

    case PARSE_ANNOT_INFER: {
        size_t at = args->n > 1 ? 1 : 0;

        if (at) {
            a->name_ref = arg_ref(args, 0);
        }

        if (arg_is(args, at, AA_INFER)) {
            a->infer_ast           = args->items.data[at]->infer;
            args->items.data[at]->infer = nullptr;
        }
        else {
            a->infer_ast = parse_infer_parse(args->items.data[at]->str);
        }

        if (!a->infer_ast) {
            return false;
        }

        char *text = parse_infer_print(a->infer_ast);

        a->infer_expr = core_string_from_raw(text, (int64_t)strlen(text));
        break;
    }

    default:
        a->name_ref = arg_ref(args, 0);
        break;
    }

    return true;
}

bool
parse_annot_parse_args(parse_annotation_t *a, core_string_t text)
{
    core_string_t nt_name = a ? parse_annot_arg_grammar(a->kind)
                             : (core_string_t){0};

    if (!nt_name.data) {
        return false;
    }

    parse_grammar_t *g = parse_annot_grammar();

    if (!g) {
        return false;
    }

    parse_nonterm_t *start = parse_find_nonterm(g, nt_name);

    if (!start || !core_list_len(g->rules)) {
        return false;
    }

    core_buffer_t       *buf = nullptr;
    parse_scanner_t      *sc  = nullptr;
    parse_token_stream_t *ts  = parse_annot_lex(text, &buf, &sc);

    if (!ts) {
        return false;
    }

    parse_pwz_parser_t *p
        = parse_pwz_new_for_start(g, parse_nonterm_id(start));
    bool              ok = false;

    if (parse_pwz_parse(p, ts)) {
        aa_ctx             c  = {
            .region = text,
            .nodes = core_list_new(aa_node_ptr_t),
        };
        parse_infer_lower_t *il = parse_infer_lower_new();
        parse_tree_walk_t   *w  = parse_tree_walk_new(g, &c);

        install(w, &c);
        parse_infer_install(w, il);

        aa_node *args = as_aa(parse_tree_walk_reduce(w, parse_pwz_get_tree(p)));

        parse_tree_walk_free(w);

        if (!c.failed && args && args->tag == AA_ARGS) {
            ok = decode(a, args);

            if (ok) {
                a->args_text = dup(text);
            }
        }

        free_args(&c);
        parse_infer_lower_free(il);
    }

    parse_pwz_free(p);
    parse_annot_lex_free(ts, buf, sc);

    return ok;
}
