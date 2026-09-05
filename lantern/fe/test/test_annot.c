#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    char *p = malloc((size_t)n + 1);

    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        p[0] = 0;
    }
    else {
        p[n] = 0;
    }

    fclose(f);

    return ncc_string_from_cstr(p);
}

typedef struct {
    int from_tree_calls;
    int from_spec_calls;
} binder_log_t;

static tdesc_t *
tag_desc(char tag)
{
    tdesc_t *t = ncc_alloc(tdesc_t);

    memset(t, 0, sizeof(*t));
    t->ext = (void *)(uintptr_t)(unsigned char)tag;

    return t;
}

static char
desc_tag(tdesc_t *t)
{
    return t ? (char)(uintptr_t)t->ext : 0;
}

static tdesc_t *
bind_tree(void *ctx, ncc_grammar_t *g, ncc_parse_tree_t *node)
{
    (void)g;
    ((binder_log_t *)ctx)->from_tree_calls++;

    ncc_string_t s = ncc_tree_extract_first_identifier(node);

    return tag_desc(s.u8_bytes ? s.data[0] : '?');
}

static tdesc_t *
bind_spec(void *ctx, ncc_string_t spec)
{
    ((binder_log_t *)ctx)->from_spec_calls++;

    return tag_desc(spec.u8_bytes ? spec.data[0] : '?');
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
        if (ncc_string_eq(g->nt_list.data[i].name, S("prog"))) {
            prog = &g->nt_list.data[i];
        }
    }

    check("the start non-terminal exists", prog != nullptr);

    if (prog) {
        check("and carries its @scope annotation",
              ncc_nt_find_annot(prog, NCC_ANNOT_SCOPE_OPEN) != nullptr);
    }

    ncc_annotation_t *a = ncc_nt_find_annot(prog, NCC_ANNOT_SCOPE_OPEN);

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

    binder_log_t      log    = {0};
    ncc_type_binder_t binder = {
        .from_tree = bind_tree,
        .from_spec = bind_spec,
        .ctx       = &log,
    };

    probe_t             probe = {0};
    ncc_annot_result_t *r     = ncc_alloc(ncc_annot_result_t);

    memset(r, 0, sizeof(*r));
    r->symtab    = ncc_symtab_new();
    r->cf_labels = ncc_alloc(ncc_dict_t);
    ncc_dict_init(r->cf_labels, ncc_hash_word, nullptr);

    ncc_tree_walk_t *w = ncc_tree_walk_new(g, r);

    ncc_annot_register(w, r, &binder);

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

    check("@declares($name,$type) binds a type through the binder",
          a && desc_tag(a->type) == 'T');
    check("in the scope where it was written",
          b_from_block && desc_tag(b_from_block->type) == 'U');
    check("@type(\"spec\") binds through the spec binder",
          c && desc_tag(c->type) == 'i');
    check("both binder paths were used",
          log.from_tree_calls == 2 && log.from_spec_calls == 1);

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
    enum { N = 4000 };

    char *src = malloc(N * 4 + 1);

    for (int i = 0; i < N; i++) {
        memcpy(src + i * 4, "vaT;", 4);
    }

    src[N * 4] = 0;

    ncc_pwz_parser_t   *p;
    ncc_token_stream_t *ts;
    ncc_parse_tree_t   *tree = parse_toy(g, src, &p, &ts);

    if (!tree) {
        check("a 4000-declaration program parses", false);
        free(src);

        return;
    }

    probe_t          probe = {0};
    ncc_tree_walk_t *w     = ncc_tree_walk_new(g, nullptr);

    ncc_walk_on_nt(w, S("decl"), saw_decl, nullptr, &probe);
    ncc_tree_walk_run(w, tree);
    ncc_tree_walk_free(w);

    check("a walk over a 4000-deep tree visits every node",
          probe.decls_seen == N);

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    free(src);
}

int
main(void)
{
    ncc_grammar_t *g = load_toy();

    if (!g) {
        return 1;
    }

    test_annotations_are_retained_on_the_grammar(g);
    test_the_walk_builds_a_symbol_table(g);
    test_a_deep_tree_does_not_overflow_the_stack(g);

    ncc_grammar_free(g);

    if (!fails) {
        printf("fe annot: all checks passed\n");
    }

    return fails ? 1 : 0;
}
