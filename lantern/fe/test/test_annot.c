#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/typelang.h"
#include "lib/alloc.h"
#include "parse/annot_walk.h"
#include "parse/annotation.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "scanner/token_stream.h"
#include "internal/parse/grammar_internal.h"

static int fails = 0;

static void
check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static ncc_string_t
S(const char *s)
{
    return ncc_string_from_cstr(s);
}

static ncc_string_t
slurp(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        printf("FAIL  cannot open %s\n", path);
        fails++;

        return (ncc_string_t){0};
    }

    fseek(f, 0, SEEK_END);

    long n = ftell(f);

    rewind(f);

    char *p = ncc_alloc_size(1, (size_t)n + 1);

    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        p[0] = 0;
    }
    else {
        p[n] = 0;
    }

    fclose(f);

    return ncc_string_from_cstr(p);
}

static th_ctx *types;

static bool
is_type(th_type *t, const char *typelang)
{
    return t && types
        && th_hash(types, t) == th_hash(types, ir_type_from_text(
                                                   types,
                                                   S(typelang)));
}

typedef struct {
    int               decls_seen;
    ncc_parse_tree_t *block_node;
    ncc_parse_tree_t *cond_node;
    ncc_scope_t      *module_scope;
    ncc_scope_t      *block_scope;
} probe_t;

static bool
saw_decl(ncc_visit_t *v)
{
    ((probe_t *)v->thunk)->decls_seen++;

    return true;
}

static bool
saw_block(ncc_visit_t *v)
{
    probe_t *p = (probe_t *)v->thunk;

    p->block_node  = v->node;
    p->block_scope = v->pn->scope;

    return true;
}

static bool
saw_prog(ncc_visit_t *v)
{
    ((probe_t *)v->thunk)->module_scope = v->pn->scope;

    return true;
}

static bool
saw_cond(ncc_visit_t *v)
{
    ((probe_t *)v->thunk)->cond_node = v->node;

    return true;
}

static ncc_grammar_t *
load_toy(void)
{
    ncc_string_t text = slurp("toy.bnf");

    if (!text.data) {
        return nullptr;
    }

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    if (!ncc_bnf_load(text, S("prog"), g)) {
        check("toy.bnf loads", false);
        ncc_grammar_free(g);

        return nullptr;
    }

    return g;
}

static ncc_parse_tree_t *
parse_toy(ncc_grammar_t *g, const char *src, ncc_pwz_parser_t **p_out,
          ncc_token_stream_t **ts_out)
{
    ncc_token_stream_t *ts = ncc_token_stream_from_codepoints(S(src));
    ncc_pwz_parser_t   *p  = ncc_pwz_new(g);

    if (!ncc_pwz_parse(p, ts)) {
        return nullptr;
    }

    *p_out  = p;
    *ts_out = ts;

    return ncc_pwz_get_tree(p);
}

static void
test_annotations_are_retained_on_the_grammar(ncc_grammar_t *g)
{
    ncc_annotation_t *tok = ncc_grammar_find_annot(g, NCC_ANNOT_TOKENIZER);

    check("a file-level annotation is retained on the grammar", tok != nullptr);

    if (tok) {
        check("and keeps its argument", ncc_string_eq(tok->scope_tag, S("toy")));
    }

    ncc_nonterm_t *prog = nullptr;

    for (size_t i = 0; i < ncc_list_len(g->nt_list); i++) {
        if (ncc_string_eq(g->nt_list.data[i]->name, S("prog"))) {
            prog = g->nt_list.data[i];
        }
    }

    check("the start non-terminal exists", prog != nullptr);

    if (prog) {
        check("and carries its @scope annotation",
              ncc_nt_find_annot(g, prog, NCC_ANNOT_SCOPE_OPEN) != nullptr);
    }

    ncc_annotation_t *a = ncc_nt_find_annot(g, prog, NCC_ANNOT_SCOPE_OPEN);

    if (a) {
        check("with the tag it was written with",
              ncc_string_eq(a->scope_tag, S("module")));
    }
}

static void
test_the_walk_builds_a_symbol_table(ncc_grammar_t *g)
{
    ncc_pwz_parser_t   *p;
    ncc_token_stream_t *ts;
    ncc_parse_tree_t   *tree = parse_toy(g, "vaT;{vbU;}?xyztcV;", &p, &ts);

    if (!tree) {
        check("the toy program parses", false);

        return;
    }

    types = th_ctx_new();

    probe_t             probe = {0};
    ncc_annot_result_t *r     = ncc_alloc(ncc_annot_result_t);

    memset(r, 0, sizeof(*r));
    r->symtab    = ncc_symtab_new();
    r->cf_labels = ncc_alloc(ncc_dict_t);
    ncc_dict_init(r->cf_labels, ncc_hash_word, nullptr);

    ncc_tree_walk_t *w = ncc_tree_walk_new(g, r);

    ncc_annot_register(w, r, types);

    check("a visitor can be registered by non-terminal name",
          ncc_walk_on_nt(w, S("decl"), saw_decl, nullptr, &probe));
    check("registering an unknown non-terminal fails",
          !ncc_walk_on_nt(w, S("no-such-rule"), saw_decl, nullptr, &probe));

    ncc_walk_on_nt(w, S("block"), nullptr, saw_block, &probe);
    ncc_walk_on_nt(w, S("prog"), nullptr, saw_prog, &probe);
    ncc_walk_on_nt(w, S("cond"), nullptr, saw_cond, &probe);

    ncc_tree_walk_run(w, tree);
    ncc_tree_walk_free(w);

    check("a caller's visitor fires once per matching node",
          probe.decls_seen == 2);
    check("the module scope was opened", probe.module_scope != nullptr);
    check("the block scope was opened", probe.block_scope != nullptr);

    if (probe.module_scope && probe.block_scope) {
        check("the block scope nests inside the module scope",
              probe.block_scope->parent == probe.module_scope);
        check("and the module scope is tagged as the annotation wrote it",
              ncc_string_eq(probe.module_scope->scope_tag, S("module")));
        check("scope depth counts from the file scope",
              probe.module_scope->depth == 1 && probe.block_scope->depth == 2);
    }

    ncc_symtab_t *st = r->symtab;

    ncc_sym_entry_t *a = ncc_symtab_lookup_scoped(st, probe.module_scope,
                                                  ncc_string_empty(), S("a"));
    ncc_sym_entry_t *b_from_module
        = ncc_symtab_lookup_scoped(st, probe.module_scope, ncc_string_empty(),
                                   S("b"));
    ncc_sym_entry_t *b_from_block
        = ncc_symtab_lookup_scoped(st, probe.block_scope, ncc_string_empty(),
                                   S("b"));
    ncc_sym_entry_t *a_from_block
        = ncc_symtab_lookup_scoped(st, probe.block_scope, ncc_string_empty(),
                                   S("a"));
    ncc_sym_entry_t *c = ncc_symtab_lookup_scoped(st, probe.module_scope,
                                                  ncc_string_empty(), S("c"));

    check("a symbol declared at module scope is found there", a != nullptr);
    check("a symbol declared in a block is not visible outside it",
          b_from_module == nullptr);
    check("but is visible from inside the block", b_from_block != nullptr);
    check("and the enclosing scope is still reachable from the block",
          a_from_block == a);
    check("a scope is still searchable after the walk popped it",
          b_from_block != nullptr && b_from_block->scope == probe.block_scope);
    check("every declaration was recorded", c != nullptr);

    check("@type's argument is typelang, so it becomes that type",
          is_type(c->type, "int"));
    check("a @declares type child that is not typelang gets no type",
          a && a->type == nullptr);

    check("an annotation can read what a later-written annotation produced",
          c && c->type != nullptr);

    check("the @branch node was found", probe.cond_node != nullptr);

    ncc_cf_label_t *cf = ncc_cf_label_lookup(r, probe.cond_node);

    check("@branch produces a control flow label", cf != nullptr);

    if (cf) {
        check("of the right kind", cf->kind == NCC_CF_BRANCH);
        check("with all three arms resolved",
              cf->cond && cf->then_body && cf->else_body
                  && cf->cond != cf->then_body
                  && cf->then_body != cf->else_body);
        check("and each arm is the child the annotation named",
              ncc_string_eq(ncc_tree_extract_first_identifier(cf->cond), S("x"))
                  && ncc_string_eq(
                      ncc_tree_extract_first_identifier(cf->then_body), S("y"))
                  && ncc_string_eq(
                      ncc_tree_extract_first_identifier(cf->else_body),
                      S("z")));
    }

    ncc_annot_result_free(r);
    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
}

static void
test_a_deep_tree_does_not_overflow_the_stack(ncc_grammar_t *g)
{
    const char *want = getenv("IR_DEEP");
    const int   N    = want ? atoi(want) : 600;

    char *src = ncc_alloc_size(1, (size_t)N * 4 + 1);

    for (int i = 0; i < N; i++) {
        memcpy(src + i * 4, "vaT;", 4);
    }

    src[N * 4] = 0;

    ncc_pwz_parser_t   *p;
    ncc_token_stream_t *ts;
    ncc_parse_tree_t   *tree = parse_toy(g, src, &p, &ts);

    if (!tree) {
        check("a deeply nested program parses", false);
        ncc_free(src);

        return;
    }

    probe_t          probe = {0};
    ncc_tree_walk_t *w     = ncc_tree_walk_new(g, nullptr);

    ncc_walk_on_nt(w, S("decl"), saw_decl, nullptr, &probe);
    ncc_tree_walk_run(w, tree);
    ncc_tree_walk_free(w);

    check("a walk over a deeply nested tree visits every node",
          probe.decls_seen == N);

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_free(src);
}

static ncc_nonterm_t *
nt_named(ncc_grammar_t *g, const char *name)
{
    for (size_t i = 0; i < ncc_list_len(g->nt_list); i++) {
        if (ncc_string_eq(g->nt_list.data[i]->name, S(name))) {
            return g->nt_list.data[i];
        }
    }

    return nullptr;
}

static void
test_annotation_arguments_only_need_balanced_parens(void)
{
    static const char *src
        = "<top> @declares($1,$2,\"var\") @disambig(pred(a, b, c()), 4) "
          "::= <n> <t>\n"
          "<n> ::= __LOWER\n"
          "<t> ::= __UPPER\n";

    ncc_grammar_t *g = ncc_grammar_new();

    check("an annotation argument may nest balanced parentheses",
          ncc_bnf_load(S(src), S("top"), g));

    ncc_nonterm_t *top = nt_named(g, "top");

    check("the annotated non-terminal is there", top != nullptr);

    if (top) {
        ncc_annotation_t *d = ncc_nt_find_annot(g, top, NCC_ANNOT_DECLARES);

        check("@declares still reads its child references",
              d != nullptr && d->name_ref.kind == NCC_ROLE_BY_INDEX
                  && d->name_ref.index == 1
                  && d->type_ref.index == 2
                  && ncc_string_eq(d->sym_kind, S("var")));

        check("the annotation that nests parentheses survives the load",
              ncc_nt_find_annot(g, top, NCC_ANNOT_DISAMBIG) != nullptr);
    }

    ncc_grammar_free(g);

    static const char *by_name
        = "<top> @literal(\"int\", $t) ::= <n> <t>\n"
          "<n> ::= __LOWER\n"
          "<t> ::= __UPPER\n";

    ncc_grammar_t *g2 = ncc_grammar_new();

    check("a reference may name a non-terminal instead of an index",
          ncc_bnf_load(S(by_name), S("top"), g2));

    ncc_nonterm_t    *t2 = nt_named(g2, "top");
    ncc_annotation_t *l  = t2 ? ncc_nt_find_annot(g2, t2, NCC_ANNOT_LITERAL)
                              : nullptr;

    check("and $name carries the name, not an index",
          l != nullptr && l->type_ref.kind == NCC_ROLE_BY_NAME
              && ncc_string_eq(l->type_ref.name, S("t"))
              && ncc_string_eq(l->op_kind, S("int")));

    ncc_grammar_free(g2);

    static const char *unbalanced
        = "<top> @disambig(pred(a) ::= <n>\n"
          "<n> ::= __LOWER\n";

    ncc_grammar_t *bad = ncc_grammar_new();

    check("an unbalanced argument region is not a grammar",
          !ncc_bnf_load(S(unbalanced), S("top"), bad));

    ncc_grammar_free(bad);
}

static void
test_annotation_arguments_carry_integers(void)
{
    static const char *src
        = "<top> @penalty(7) ::= <n>\n"
          "<mid> @disambig(pred, 3) ::= <n>\n"
          "<n> ::= __LOWER\n";

    ncc_grammar_t *g = ncc_grammar_new();

    check("a grammar with integer annotation arguments loads",
          ncc_bnf_load(S(src), S("top"), g));

    ncc_nonterm_t *top = nt_named(g, "top");
    ncc_nonterm_t *mid = nt_named(g, "mid");

    ncc_annotation_t *p = top ? ncc_nt_find_annot(g, top, NCC_ANNOT_PENALTY)
                              : nullptr;

    check("@penalty reads the number it was written with",
          p != nullptr && p->penalty_cost == 7);

    ncc_annotation_t *d = mid ? ncc_nt_find_annot(g, mid, NCC_ANNOT_DISAMBIG)
                              : nullptr;

    check("@disambig reads its predicate and its cost",
          d != nullptr && ncc_string_eq(d->disambig_predicate, S("pred"))
              && d->disambig_cost == 3);

    ncc_grammar_free(g);

    static const char *const BAD[] = {
        "<top> @penalty(-4) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @penalty(a) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @penalty(1, 2) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @branch($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @assigns($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @scope($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @jump($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @adt(\"enum\", $1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @nospace($1, $2) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @declares(\"v\", $1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @literal(\"int\", t) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @nospace(n) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @varref($) ::= <n>\n<n> ::= __LOWER\n",
    };

    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        ncc_grammar_t *bad = ncc_grammar_new();

        if (ncc_bnf_load(S(BAD[i]), S("top"), bad)) {
            printf("FAIL  this should not have loaded: %s", BAD[i]);
            fails++;
        }

        ncc_grammar_free(bad);
    }
}

static void
test_the_representation_and_declaration_annotations(void)
{
    static const char *const G
        = "<top> @scope(\"module\") ::= <d> <b> <p>\n"
          "<d> @declares($1,\"var\") @mutability($1,\"const\") "
          "@constrain($1, == int) @namespace(\"tag\") ::= \"v\" <n> \";\"\n"
          "<b> @size($2) @width($2) ::= \"b\" %\":\" <num>\n"
          "<p> @declares($1,\"param\") @direction($1,\"out\") "
          "@alignment(8) @offset(sizeof(int)) ::= \"p\" <n> \";\"\n"
          "<n> ::= __LOWER\n"
          "<num> ::= __DIGIT\n";

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    check("a grammar using every Part 2 annotation loads",
          ncc_bnf_load(S(G), S("top"), g));

    ncc_pwz_parser_t   *p;
    ncc_token_stream_t *ts;
    ncc_parse_tree_t   *tree = parse_toy(g, "va;b:4pq;", &p, &ts);

    if (!tree) {
        check("the program parses", false);
        ncc_grammar_free(g);

        return;
    }

    types = types ? types : th_ctx_new();

    ncc_annot_result_t *r = ncc_annot_walk_tree(g, tree, types);

    ncc_sym_entry_t *a = nullptr;
    ncc_sym_entry_t *q = nullptr;

    for (ncc_scope_t *s = r->symtab->all_scopes; s; s = s->all_next) {
        for (ncc_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->name.data && !strcmp(e->name.data, "a")) {
                a = e;
            }

            if (e->name.data && !strcmp(e->name.data, "q")) {
                q = e;
            }
        }
    }

    check("@mutability sets the symbol's mutability",
          a && a->mutability == NCC_SYM_CONST);
    check("@constrain records the constraint as written",
          a && a->constraints.data && !strcmp(a->constraints.data, "== int"));
    check("@direction records the parameter's direction",
          q && q->direction.data && !strcmp(q->direction.data, "out"));
    check("@namespace put the declaration in the namespace it named",
          a && a->scope && a->scope->ns
              && a->scope->ns->ns_name.data
              && !strcmp(a->scope->ns->ns_name.data, "tag"));

    ncc_parse_tree_t *b = nullptr;

    for (size_t i = 0; i < ncc_tree_num_children(tree); i++) {
        ncc_parse_tree_t *k = ncc_tree_child(tree, i);

        if (k && !ncc_tree_is_leaf(k)
            && ncc_tree_node_value(k).repr.has_size) {
            b = k;
        }
    }

    check("@size($N) read the value out of the parse",
          b && ncc_tree_node_value(b).repr.size == 4);
    check("@width($N) read the same child",
          b && ncc_tree_node_value(b).repr.has_width
              && ncc_tree_node_value(b).repr.width == 4);

    ncc_annot_result_free(r);
    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_grammar_free(g);
}

static void
test_part_two_rejections(void)
{
    static const char *const BAD[] = {
        "<top> @mutability($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @namespace($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @direction(\"out\") ::= <n>\n<n> ::= __LOWER\n",
        "<top> @constrain($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @constrain($1, ?? int) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @size() ::= <n>\n<n> ::= __LOWER\n",
        "<top> @size(sizeof) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @alignment(1, 2) ::= <n>\n<n> ::= __LOWER\n",
    };

    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        ncc_grammar_t *bad = ncc_grammar_new();

        if (ncc_bnf_load(S(BAD[i]), S("top"), bad)) {
            printf("FAIL  this should not have loaded: %s", BAD[i]);
            fails++;
        }

        ncc_grammar_free(bad);
    }
}

int
main(void)
{
    ncc_grammar_t *g = load_toy();

    if (!g) {
        return 1;
    }

    test_annotation_arguments_only_need_balanced_parens();
    test_annotation_arguments_carry_integers();
    test_the_representation_and_declaration_annotations();
    test_part_two_rejections();
    test_annotations_are_retained_on_the_grammar(g);
    test_the_walk_builds_a_symbol_table(g);
    test_a_deep_tree_does_not_overflow_the_stack(g);

    ncc_grammar_free(g);

    if (!fails) {
        printf("fe annot: all checks passed\n");
    }

    return fails ? 1 : 0;
}
