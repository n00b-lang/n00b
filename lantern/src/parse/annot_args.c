#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "private/parse/annot_grammar.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"

#include <string.h>

#define AA_MAGIC 0x41413031u

typedef enum : uint8_t {
    AA_REF,
    AA_TAG,
    AA_INT,
    AA_LEAF,
    AA_ARGS,
} aa_tag;

typedef struct aa_node *aa_node_ptr_t;
core_list_decl(aa_node_ptr_t);

typedef struct aa_node {
    uint32_t      magic;
    aa_tag        tag;
    core_string_t str;
    int32_t       i;
    core_list_t(aa_node_ptr_t) items;
} aa_node;

typedef struct {
    core_list_t(aa_node_ptr_t) nodes;
} aa_ctx;

static aa_node *
aa_new(parse_reduce_t *r, aa_tag tag)
{
    aa_node *n = core_alloc(aa_node);
    aa_ctx  *c = r->thunk;

    n->magic = AA_MAGIC;
    n->tag   = tag;
    core_list_push(c->nodes, n);

    return n;
}

static aa_node *
as_aa(void *v)
{
    aa_node *n = v;

    return (n && !parse_reduce_as_list(v) && n->magic == AA_MAGIC) ? n
                                                                   : nullptr;
}

static int32_t
leaf_int(aa_node *leaf)
{
    return leaf && leaf->str.data ? (int32_t)core_string_strtoll(leaf->str, 10)
                                  : 0;
}

static void *
r_leaf(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_LEAF);

    n->str = parse_tree_leaf_text(r->node);

    return n;
}

static void *
r_ref(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_REF);

    n->i = leaf_int(r->n_kids > 1 ? as_aa(r->kids[1]) : nullptr);

    return n;
}

static void *
r_tag(parse_reduce_t *r)
{
    aa_node *n  = aa_new(r, AA_TAG);
    aa_node *lf = r->n_kids ? as_aa(r->kids[0]) : nullptr;

    n->str = lf ? lf->str : (core_string_t){0};

    return n;
}

static void *
r_int(parse_reduce_t *r)
{
    aa_node *n = aa_new(r, AA_INT);

    n->i = leaf_int(r->n_kids ? as_aa(r->kids[0]) : nullptr);

    return n;
}

static void
append_arg(aa_node *args, void *v)
{
    aa_node *item = as_aa(v);

    if (item) {
        if (item->tag != AA_LEAF) core_list_push(args->items, item);
        return;
    }

    parse_reduce_list_t *list = parse_reduce_as_list(v);
    for (size_t i = 0; list && i < list->n; i++) append_arg(args, list->v[i]);
}

static void *
r_args(parse_reduce_t *r)
{
    aa_node *args = aa_new(r, AA_ARGS);

    args->items = core_list_new(aa_node_ptr_t);
    for (size_t i = 0; i < r->n_kids; i++) append_arg(args, r->kids[i]);

    return args;
}

static struct {
    parse_annot_kind_t kind;
    core_string_t      nt;
} const SIGS[] = {
    {PARSE_ANNOT_SCOPE_OPEN,        CORE_STRING_STATIC("a-scope")},
    {PARSE_ANNOT_DECLARES,          CORE_STRING_STATIC("a-declares")},
    {PARSE_ANNOT_TYPE,              CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_ASSIGNS,           CORE_STRING_STATIC("a-assigns")},
    {PARSE_ANNOT_BRANCH,            CORE_STRING_STATIC("a-branch")},
    {PARSE_ANNOT_LOOP,              CORE_STRING_STATIC("a-pair")},
    {PARSE_ANNOT_SWITCH,            CORE_STRING_STATIC("a-pair")},
    {PARSE_ANNOT_CALL,              CORE_STRING_STATIC("a-pair")},
    {PARSE_ANNOT_FIELD,             CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_JUMP,              CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_TOKENIZER,         CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_VISIBILITY,        CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_OPERATOR,          CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_DISAMBIG,          CORE_STRING_STATIC("a-disambig")},
    {PARSE_ANNOT_EXPOSES,           CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_VARREF,            CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_INDENT,            CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_TRIVIA_INDENT,     CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_GROUP,             CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_ALIGN,             CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_SPACE,             CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_NOSPACE,           CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_ADJACENT,          CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_NEWLINE,           CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_HARDLINE,          CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_BLANKLINE,         CORE_STRING_STATIC("a-ref")},
    {PARSE_ANNOT_NAMESPACE,         CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_DIRECTION,         CORE_STRING_STATIC("a-direction")},
    {PARSE_ANNOT_PASS_BY,           CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_LEXER,             CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_DECLARATION_POINT, CORE_STRING_STATIC("a-tag")},
    {PARSE_ANNOT_STATEMENT_END,     CORE_STRING_STATIC("a-tags")},
    {PARSE_ANNOT_CONTINUE_AFTER,    CORE_STRING_STATIC("a-tags")},
    {PARSE_ANNOT_DISCARDS,          CORE_STRING_STATIC("a-ref")},
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
    parse_walk_reduce_leaf_in(w,
                              parse_nt_unit(parse_annot_grammar(),
                                            CORE_STRING_STATIC("a-ref")),
                              r_leaf, c);
    parse_walk_reduce_nt(w, CORE_STRING_STATIC("ref"), r_ref, c);
    parse_walk_reduce_nt(w, CORE_STRING_STATIC("tag"), r_tag, c);
    parse_walk_reduce_nt(w, CORE_STRING_STATIC("name"), r_tag, c);
    parse_walk_reduce_nt(w, CORE_STRING_STATIC("num"), r_int, c);

    for (size_t i = 0; i < sizeof(SIGS) / sizeof(SIGS[0]); i++) {
        parse_walk_reduce_nt(w, SIGS[i].nt, r_args, c);
    }
}

static bool
decode(parse_annotation_t *a, aa_node *args)
{
    int32_t *refs[] = {&a->name_ref, &a->type_ref, &a->value_ref};
    size_t   n_refs = 0;
    bool     listed = a->kind == PARSE_ANNOT_STATEMENT_END
                   || a->kind == PARSE_ANNOT_CONTINUE_AFTER;

    if (a->kind == PARSE_ANNOT_ASSIGNS) refs[1] = &a->value_ref;
    if (listed) a->tags = core_list_new(core_string_t);
    a->cost = 1;

    for (size_t i = 0; i < args->items.len; i++) {
        aa_node *item = args->items.data[i];

        switch (item->tag) {
        case AA_REF:
            *refs[n_refs++] = item->i;
            break;
        case AA_TAG:
            if (listed) core_list_push(a->tags, core_string_dup(item->str));
            else a->tag = core_string_dup(item->str);
            break;
        case AA_INT:
            if (item->i > 0) a->cost = item->i;
            break;
        default:
            break;
        }
    }

    switch (a->kind) {
    case PARSE_ANNOT_DECLARATION_POINT:
        return core_string_eq(a->tag, CORE_STRING_STATIC("declarator"))
            || core_string_eq(a->tag, CORE_STRING_STATIC("after_initializer"));
    case PARSE_ANNOT_ADJACENT:
        return core_string_eq(a->tag, CORE_STRING_STATIC("prefix"));
    default:
        return true;
    }
}

bool
parse_annot_parse_args(parse_annotation_t *a, core_string_t text)
{
    core_string_t nt_name = a ? parse_annot_arg_grammar(a->kind)
                              : (core_string_t){0};
    parse_grammar_t *g     = nt_name.data ? parse_annot_grammar() : nullptr;
    parse_nonterm_t *start = g ? parse_find_nonterm(g, nt_name) : nullptr;

    if (!start || !core_list_len(g->rules)) {
        return false;
    }

    core_buffer_t        *buf = nullptr;
    parse_scanner_t      *sc  = nullptr;
    parse_token_stream_t *ts  = parse_annot_lex(text, &buf, &sc);

    if (!ts) {
        return false;
    }

    parse_pwz_parser_t *p  = parse_pwz_new_for_start(g, parse_nonterm_id(start));
    bool                ok = false;

    if (parse_pwz_parse(p, ts)) {
        aa_ctx             c = {.nodes = core_list_new(aa_node_ptr_t)};
        parse_tree_walk_t *w = parse_tree_walk_new(g, &c);

        install(w, &c);

        aa_node *args = as_aa(parse_tree_walk_reduce(w, parse_pwz_get_tree(p)));

        parse_tree_walk_free(w);

        if (args && args->tag == AA_ARGS) {
            ok = decode(a, args);

            if (ok) {
                a->args_text = core_string_dup(text);
            }
        }

        for (size_t i = 0; i < c.nodes.len; i++) {
            core_list_free(c.nodes.data[i]->items);
            core_free(c.nodes.data[i]);
        }
        core_list_free(c.nodes);
    }

    parse_pwz_free(p);
    parse_annot_lex_free(ts, buf, sc);

    return ok;
}
