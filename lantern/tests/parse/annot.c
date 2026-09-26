#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ir/typelang.h"
#include "ir/layout.h"
#include "lib/alloc.h"
#include "parse/annot_walk.h"
#include "parse/annotation.h"
#include "parse/bnf.h"
#include "parse/infer.h"
#include "parse/pwz.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

static core_string_t
S(const char *s)
{
    return core_string_from_cstr(s);
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
    parse_tree_t *block_node;
    parse_tree_t *cond_node;
    parse_scope_t      *module_scope;
    parse_scope_t      *block_scope;
} probe_t;

static bool
saw_decl(parse_visit_t *v)
{
    ((probe_t *)v->thunk)->decls_seen++;

    return true;
}

static bool
saw_block(parse_visit_t *v)
{
    probe_t *p = (probe_t *)v->thunk;

    p->block_node  = v->node;
    p->block_scope = v->pn->scope;

    return true;
}

static bool
saw_prog(parse_visit_t *v)
{
    ((probe_t *)v->thunk)->module_scope = v->pn->scope;

    return true;
}

static bool
saw_cond(parse_visit_t *v)
{
    ((probe_t *)v->thunk)->cond_node = v->node;

    return true;
}

typedef struct {
    size_t enters;
    size_t exits;
} node_probe_t;

static void
count_annot_nodes(parse_visit_t *v, bool entering, void *context)
{
    node_probe_t *probe = context;
    (void)v;

    if (entering) {
        probe->enters++;
    }
    else {
        probe->exits++;
    }
}

static parse_grammar_t *
load_toy(void)
{
    core_string_t text = slurp("toy.bnf");

    if (!text.data) {
        return nullptr;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    if (!parse_bnf_load(text, S("prog"), g)) {
        check("toy.bnf loads", false);
        parse_grammar_free(g);

        return nullptr;
    }

    return g;
}

static parse_tree_t *
parse_toy(parse_grammar_t *g, const char *start, const char *src,
          parse_pwz_parser_t **p_out,
          parse_token_stream_t **ts_out)
{
    parse_token_stream_t *ts = scan_token_stream_from_codepoints(S(src));
    parse_pwz_parser_t   *p = parse_pwz_new_for_start(
        g, parse_nonterm_id(parse_find_nonterm(g, S(start))));

    if (!parse_pwz_parse(p, ts)) {
        return nullptr;
    }

    *p_out  = p;
    *ts_out = ts;

    return parse_pwz_get_tree(p);
}

static parse_tree_t *
find_node(parse_grammar_t *g, parse_tree_t *node, const char *name)
{
    if (!node || core_tree_is_leaf(node)) {
        return nullptr;
    }

    if (core_string_eq(parse_tree_nt_name(g, node),
                       core_string_borrow(name, strlen(name)))) {
        return node;
    }

    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = find_node(g, core_tree_child(node, i), name);

        if (found) {
            return found;
        }
    }

    return nullptr;
}

static void
test_annotations_are_retained_on_the_grammar(parse_grammar_t *g)
{
    parse_annotation_t *tok = parse_grammar_find_annot(g, PARSE_ANNOT_TOKENIZER);

    check("a file-level annotation is retained on the grammar", tok != nullptr);

    if (tok) {
        check("and keeps its argument", core_string_eq(tok->scope_tag, S("toy")));
    }

    parse_nonterm_t *prog = nullptr;

    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        if (core_string_eq(g->nt_list.data[i]->name, S("prog"))) {
            prog = g->nt_list.data[i];
        }
    }

    check("the start non-terminal exists", prog != nullptr);

    if (prog) {
        check("and carries its @scope annotation",
              parse_nt_find_annot(g, prog, PARSE_ANNOT_SCOPE_OPEN) != nullptr);
    }

    parse_annotation_t *a = parse_nt_find_annot(g, prog, PARSE_ANNOT_SCOPE_OPEN);

    if (a) {
        check("with the tag it was written with",
              core_string_eq(a->scope_tag, S("module")));
    }
}

typedef struct {
    parse_grammar_t *grammar;
    parse_nonterm_t *nonterm;
    bool             ok;
} annot_lookup_run_t;

static void *
lookup_annotations_concurrently(void *arg)
{
    annot_lookup_run_t *run = arg;
    run->ok = true;

    for (int i = 0; i < 2000; i++) {
        parse_annotation_t *file
            = parse_grammar_find_annot(run->grammar, PARSE_ANNOT_TOKENIZER);
        parse_annotation_t *scope
            = parse_nt_find_annot(run->grammar, run->nonterm,
                                  PARSE_ANNOT_SCOPE_OPEN);

        if (!file || !scope) {
            run->ok = false;
            break;
        }
    }

    return nullptr;
}

static void
test_annotation_cache_is_thread_safe(parse_grammar_t *g)
{
    parse_nonterm_t *prog = parse_find_nonterm(g, S("prog"));
    enum { N_THREADS = 8 };
    pthread_t threads[N_THREADS];
    annot_lookup_run_t runs[N_THREADS];
    bool created[N_THREADS] = {0};

    parse_grammar_finalize(g);

    for (int i = 0; i < N_THREADS; i++) {
        runs[i] = (annot_lookup_run_t){.grammar = g, .nonterm = prog};
        created[i] = pthread_create(&threads[i], nullptr,
                                    lookup_annotations_concurrently,
                                    &runs[i]) == 0;
    }

    bool ok = prog != nullptr;
    for (int i = 0; i < N_THREADS; i++) {
        if (!created[i]) {
            ok = false;
            continue;
        }

        pthread_join(threads[i], nullptr);
        ok &= runs[i].ok;
    }

    check("annotation cache lookups are safe across threads", ok);
}

static void
test_the_walk_builds_a_symbol_table(parse_grammar_t *g)
{
    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree
        = parse_toy(g, "prog", "vaT;{vbU;}?xyztcV;", &p, &ts);

    if (!tree) {
        check("the toy program parses", false);

        return;
    }

    types = th_ctx_new();

    probe_t             probe = {0};
    parse_annot_result_t *r     = core_alloc(parse_annot_result_t);

    memset(r, 0, sizeof(*r));
    r->symtab    = parse_symtab_new();
    r->cf_labels = core_alloc(core_dict_t);
    core_dict_init(r->cf_labels, core_hash_word, nullptr);

    parse_tree_walk_t *w = parse_tree_walk_new(g, r);

    parse_annot_register(w, r, types, nullptr);

    check("a visitor can be registered by non-terminal name",
          parse_walk_on_nt(w, S("decl"), saw_decl, nullptr, &probe));
    check("registering an unknown non-terminal fails",
          !parse_walk_on_nt(w, S("no-such-rule"), saw_decl, nullptr, &probe));

    parse_walk_on_nt(w, S("block"), nullptr, saw_block, &probe);
    parse_walk_on_nt(w, S("prog"), nullptr, saw_prog, &probe);
    parse_walk_on_nt(w, S("cond"), nullptr, saw_cond, &probe);

    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    check("a caller's visitor fires once per matching node",
          probe.decls_seen == 2);
    check("the module scope was opened", probe.module_scope != nullptr);
    check("the block scope was opened", probe.block_scope != nullptr);

    if (probe.module_scope && probe.block_scope) {
        check("the block scope nests inside the module scope",
              probe.block_scope->parent == probe.module_scope);
        check("and the module scope is tagged as the annotation wrote it",
              core_string_eq(probe.module_scope->scope_tag, S("module")));
        check("scope depth counts from the file scope",
              probe.module_scope->depth == 1 && probe.block_scope->depth == 2);
    }

    parse_symtab_t *st = r->symtab;

    parse_sym_entry_t *a = parse_symtab_lookup_scoped(st, probe.module_scope,
                                                  core_string_empty(), S("a"));
    parse_sym_entry_t *b_from_module
        = parse_symtab_lookup_scoped(st, probe.module_scope, core_string_empty(),
                                   S("b"));
    parse_sym_entry_t *b_from_block
        = parse_symtab_lookup_scoped(st, probe.block_scope, core_string_empty(),
                                   S("b"));
    parse_sym_entry_t *a_from_block
        = parse_symtab_lookup_scoped(st, probe.block_scope, core_string_empty(),
                                   S("a"));
    parse_sym_entry_t *c = parse_symtab_lookup_scoped(st, probe.module_scope,
                                                  core_string_empty(), S("c"));

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

    parse_cf_label_t *cf = parse_cf_label_lookup(r, probe.cond_node);

    check("@branch produces a control flow label", cf != nullptr);

    if (cf) {
        check("of the right kind", cf->kind == PARSE_CF_BRANCH);
        check("with all three arms resolved",
              cf->cond && cf->then_body && cf->else_body
                  && cf->cond != cf->then_body
                  && cf->then_body != cf->else_body);
        check("and each arm is the child the annotation named",
              core_string_eq(parse_tree_extract_first_identifier(cf->cond), S("x"))
                  && core_string_eq(
                      parse_tree_extract_first_identifier(cf->then_body), S("y"))
                  && core_string_eq(
                      parse_tree_extract_first_identifier(cf->else_body),
                      S("z")));
    }

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static void
test_a_deep_tree_does_not_overflow_the_stack(parse_grammar_t *g)
{
    const char *want = getenv("IR_DEEP");
    const int   N    = want ? atoi(want) : 600;

    char *src = core_alloc_size(1, (size_t)N * 4 + 1);

    for (int i = 0; i < N; i++) {
        memcpy(src + i * 4, "vaT;", 4);
    }

    src[N * 4] = 0;

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "prog", src, &p, &ts);

    if (!tree) {
        check("a deeply nested program parses", false);
        core_free(src);

        return;
    }

    probe_t          probe = {0};
    parse_tree_walk_t *w     = parse_tree_walk_new(g, nullptr);

    parse_walk_on_nt(w, S("decl"), saw_decl, nullptr, &probe);
    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    check("a walk over a deeply nested tree visits every node",
          probe.decls_seen == N);

    parse_pwz_free(p);
    scan_token_stream_free(ts);
    core_free(src);
}

static parse_nonterm_t *
nt_named(parse_grammar_t *g, const char *name)
{
    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        if (core_string_eq(g->nt_list.data[i]->name, S(name))) {
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

    parse_grammar_t *g = parse_grammar_new();

    check("an annotation argument may nest balanced parentheses",
          parse_bnf_load(S(src), S("top"), g));

    parse_nonterm_t *top = nt_named(g, "top");

    check("the annotated non-terminal is there", top != nullptr);

    if (top) {
        parse_annotation_t *d = parse_nt_find_annot(g, top, PARSE_ANNOT_DECLARES);

        check("@declares still reads its child references",
              d != nullptr && d->name_ref.kind == PARSE_ROLE_BY_INDEX
                  && d->name_ref.index == 1
                  && d->type_ref.index == 2
                  && core_string_eq(d->sym_kind, S("var")));

        check("the annotation that nests parentheses survives the load",
              parse_nt_find_annot(g, top, PARSE_ANNOT_DISAMBIG) != nullptr);
    }

    parse_grammar_free(g);

    static const char *by_name
        = "<top> @literal(\"int\", $t) ::= <n> <t>\n"
          "<n> ::= __LOWER\n"
          "<t> ::= __UPPER\n";

    parse_grammar_t *g2 = parse_grammar_new();

    check("a reference may name a non-terminal instead of an index",
          parse_bnf_load(S(by_name), S("top"), g2));

    parse_nonterm_t    *t2 = nt_named(g2, "top");
    parse_annotation_t *l  = t2 ? parse_nt_find_annot(g2, t2, PARSE_ANNOT_LITERAL)
                              : nullptr;

    check("and $name carries the name, not an index",
          l != nullptr && l->type_ref.kind == PARSE_ROLE_BY_NAME
              && core_string_eq(l->type_ref.name, S("t"))
              && core_string_eq(l->op_kind, S("int")));

    parse_grammar_free(g2);

    static const char *unbalanced
        = "<top> @disambig(pred(a) ::= <n>\n"
          "<n> ::= __LOWER\n";

    parse_grammar_t *bad = parse_grammar_new();

    check("an unbalanced argument region is not a grammar",
          !parse_bnf_load(S(unbalanced), S("top"), bad));

    parse_grammar_free(bad);
}

static void
test_annotation_arguments_carry_integers(void)
{
    static const char *src
        = "<top> @penalty(7) ::= <n>\n"
          "<mid> @disambig(pred, 3) ::= <n>\n"
          "<n> ::= __LOWER\n";

    parse_grammar_t *g = parse_grammar_new();

    check("a grammar with integer annotation arguments loads",
          parse_bnf_load(S(src), S("top"), g));

    parse_nonterm_t *top = nt_named(g, "top");
    parse_nonterm_t *mid = nt_named(g, "mid");

    parse_annotation_t *p = top ? parse_nt_find_annot(g, top, PARSE_ANNOT_PENALTY)
                              : nullptr;

    check("@penalty reads the number it was written with",
          p != nullptr && p->penalty_cost == 7);

    parse_annotation_t *d = mid ? parse_nt_find_annot(g, mid, PARSE_ANNOT_DISAMBIG)
                              : nullptr;

    check("@disambig reads its predicate and its cost",
          d != nullptr && core_string_eq(d->disambig_predicate, S("pred"))
              && d->disambig_cost == 3);

    parse_grammar_free(g);

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
        parse_grammar_t *bad = parse_grammar_new();

        if (parse_bnf_load(S(BAD[i]), S("top"), bad)) {
            printf("FAIL  this should not have loaded: %s", BAD[i]);
            fails++;
        }

        parse_grammar_free(bad);
    }
}

static void
test_annotation_argument_lists_are_growable(void)
{
    core_buffer_t src = {0};
    static const char prefix[] = "<top> @constrain($1, ";
    core_buffer_append(&src, prefix, sizeof(prefix) - 1);
    for (size_t i = 0; i < 40; i++) {
        if (i) core_buffer_append(&src, "+", 1);
        core_buffer_printf(&src, "Trait%zu", i);
    }
    static const char suffix[] = ") ::= <n>\n<n> ::= __LOWER\n";
    core_buffer_append(&src, suffix, sizeof(suffix) - 1);

    parse_grammar_t *g = parse_grammar_new();
    bool loaded = parse_bnf_load(
        core_string_borrow(src.data, src.byte_len), S("top"), g);
    parse_nonterm_t *top = loaded ? nt_named(g, "top") : nullptr;
    parse_annotation_t *annotation = top
        ? parse_nt_find_annot(g, top, PARSE_ANNOT_CONSTRAIN) : nullptr;

    check("annotation argument lists accept more than 32 entries",
          annotation && core_list_len(annotation->constraints) == 40);

    parse_grammar_free(g);
    core_buffer_release(&src);
}

static void
test_implements_is_checked_not_carried(void)
{
    static const char *const G
        = "<top> @scope(\"module\") ::= <d>\n"
          "<d> @declares($1,\"var\") @type($1, int) @implements($2) "
          "::= \"v\" <n> <i> \";\"\n"
          "<n> ::= __LOWER\n"
          "<i> ::= __UPPER\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("an @implements grammar loads", parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "top", "vaR;", &p, &ts);

    if (!tree) {
        check("the declaration parses", false);
        parse_grammar_free(g);

        return;
    }

    th_ctx             *tc = th_ctx_new();
    parse_annot_result_t *r  = parse_annot_result_new(nullptr);

    for (int ph = PARSE_ANNOT_PASS_SCOPE; ph <= PARSE_ANNOT_PASS_INFER; ph++) {
        parse_tree_walk_t *w = parse_tree_walk_new(g, r);

        parse_annot_register_pass(w, r, tc, nullptr, (parse_annot_pass_t)ph);
        parse_tree_walk_run(w, tree);
        parse_tree_walk_free(w);
    }

    parse_sym_entry_t *d = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s && !d; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->name.data && !strcmp(e->name.data, "a")) {
                d = e;
                break;
            }
        }
    }

    check("the declaration was found", d != nullptr);
    check("@implements against an interface int cannot satisfy is an error",
          th_error_count(tc) > 0);

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_an_adt_builds_a_variant_type(void)
{
    static const char *const G
        = "<top> @scope(\"module\") ::= <adt>\n"
          "<adt> @adt(\"union\", $1, \"arm\", $0) ::= \"u\" <n> \"{\" <as> \"}\"\n"
          "<as> ::= <arm> <as> | <arm>\n"
          "<arm> @declares($1,\"var\") @type($1, int) ::= <n> \";\"\n"
          "<n> ::= __LOWER\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("an adt grammar loads", parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "top", "ut{a;b;}", &p, &ts);

    if (!tree) {
        check("the adt parses", false);
        parse_grammar_free(g);

        return;
    }

    types = types ? types : th_ctx_new();

    parse_annot_result_t *r = parse_annot_result_new(nullptr);

    for (int ph = PARSE_ANNOT_PASS_SCOPE; ph <= PARSE_ANNOT_PASS_INFER; ph++) {
        parse_tree_walk_t *w = parse_tree_walk_new(g, r);

        parse_annot_register_pass(w, r, types, nullptr,
                                (parse_annot_pass_t)ph);
        parse_tree_walk_run(w, tree);
        parse_tree_walk_free(w);
    }

    th_type *var = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s && !var; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->kind == PARSE_SYM_TAG && e->type) {
                var = e->type;
                break;
            }
        }
    }

    check("@adt gave its tag symbol a type", var != nullptr);
    bool is_variant = false;

    check("and the type reached th as a variant",
          var && th_named_info(var, nullptr, nullptr, &is_variant, nullptr)
              && is_variant);
    check("with one argument per arm", var && th_named_nargs(var) == 2);

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_the_repr_annotations_reach_the_layout_descriptor(void)
{
    static const char *const G
        = "<top> @scope(\"module\") ::= <rec> <access>\n"
          "<rec> @scope(\"record\") @record($1) @size(8) @alignment(4) "
          "::= \"r\" <n> \"{\" <fs> \"}\"\n"
          "<fs> ::= <f> <fs> | <meth> <fs> | <f> | <meth>\n"
          "<f> @field($1) @offset(0) @width(4) ::= <n> \";\"\n"
          "<meth> @method($1) @type($1, \"() -> int\") ::= \"m\" <n>\n"
          "<access> @infer(\"method_return($0)\") ::= <use> %\".\" <n>\n"
          "<use> @varref($0) ::= <n>\n"
          "<n> ::= __LOWER\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    bool grammar_loaded = parse_bnf_load(S(G), S("top"), g);
    check("a record grammar with representation pins loads", grammar_loaded);
    if (!grammar_loaded) {
        fprintf(stderr, "record grammar load error: %s\n", parse_bnf_error());
        parse_grammar_free(g);
        return;
    }

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "top", "rt{a;mf}t.f", &p, &ts);

    if (!tree) {
        check("the record parses", false);
        parse_grammar_free(g);

        return;
    }

    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "host");

    parse_annot_result_t *r = parse_annot_result_new(nullptr);

    for (int ph = PARSE_ANNOT_PASS_SCOPE; ph <= PARSE_ANNOT_PASS_INFER; ph++) {
        parse_tree_walk_t *w = parse_tree_walk_new(g, r);

        parse_annot_register_pass(w, r, ir_module_types(m), m,
                                (parse_annot_pass_t)ph);
        parse_tree_walk_run(w, tree);
        parse_tree_walk_free(w);
    }

    th_type *rec = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s && !rec; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->type && th_kind_of(e->type) == TH_RECORD) {
                rec = e->type;
                break;
            }
        }
    }

    check("@record established a record type", rec != nullptr);

    ir_descriptor_t *d = rec ? ir_layout_of(m, rec, nullptr) : nullptr;

    check("the annotations reached a layout descriptor", d != nullptr);
    check("@size pinned the descriptor's size",
          d && ir_layout_is_settled(&d->size) && d->size.value == 8);
    check("@alignment pinned the descriptor's alignment",
          d && ir_layout_is_settled(&d->align) && d->align.value == 4);

    th_type *method = rec ? th_method_lookup(ir_module_types(m), rec, "f")
                          : nullptr;
    th_type *method_return = method ? th_func_ret(method) : nullptr;
    const char *return_name = nullptr;
    size_t return_name_len = 0;

    check("@method registers its signature on the record type",
          method != nullptr);
    check("the registered method preserves its return type",
          method_return
              && th_named_info(method_return, &return_name, &return_name_len,
                               nullptr, nullptr)
              && return_name_len == 3 && !memcmp(return_name, "int", 3));

    parse_tree_t *access = find_node(g, tree, "access");
    th_type *access_type = access ? core_tree_node_value(access).type : nullptr;
    const char *access_name = nullptr;
    size_t access_name_len = 0;

    check("method_return resolves through the receiver method registry",
          access_type
              && th_named_info(access_type, &access_name, &access_name_len,
                               nullptr, nullptr)
              && access_name_len == 3 && !memcmp(access_name, "int", 3));

    parse_annot_result_free(r);
    ir_ctx_free(ctx);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
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
          "@alignment(8) @offset(sizeof(int)) @pass_by(\"ref\") ::= \"p\" <n> \";\"\n"
          "<n> ::= __LOWER\n"
          "<num> ::= __DIGIT\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("a grammar using every Part 2 annotation loads",
          parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "top", "va;b:4pq;", &p, &ts);

    if (!tree) {
        check("the program parses", false);
        parse_grammar_free(g);

        return;
    }

    types = types ? types : th_ctx_new();

    parse_annot_result_t *r = parse_annot_walk_tree(g, tree, types, nullptr);

    parse_sym_entry_t *a = nullptr;
    parse_sym_entry_t *q = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->name.data && !strcmp(e->name.data, "a")) {
                a = e;
            }

            if (e->name.data && !strcmp(e->name.data, "q")) {
                q = e;
            }
        }
    }

    check("@mutability sets the symbol's mutability",
          a && a->mutability == PARSE_SYM_CONST);
    th_constraint con  = {0};
    bool          got  = a && a->type
                      && th_var_constraint_count(a->type) == 1
                      && th_var_constraint_at(a->type, 0, &con);

    check("@constrain put a constraint on the symbol's type variable", got);
    check("and it is the == constraint the annotation wrote",
          got && con.kind == TH_CON_UNIFIES);
    check("@direction records the parameter's direction as a th_mode",
          q && q->has_mode && q->mode == TH_OUT);
    check("@direction preserves the source-level mode spelling",
          q && core_string_eq(q->direction, S("out")));
    check("@pass_by records a separate parameter passing strategy",
          q && core_string_eq(q->pass_by, S("ref")));
    check("@namespace put the declaration in the namespace it named",
          a && a->scope && a->scope->ns
              && a->scope->ns->ns_name.data
              && !strcmp(a->scope->ns->ns_name.data, "tag"));

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_initial_symbol_walk_callback(parse_grammar_t *g)
{
    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "prog", "vaT;{vbU;}?xyztcV;",
                                   &p, &ts);
    check("initial-symbol sample parses", tree != nullptr);
    if (!tree) return;

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    node_probe_t probe = {0};
    parse_annot_result_set_node_callback(result, count_annot_nodes, &probe);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register_pass(walk, result, nullptr, nullptr,
                              PARSE_ANNOT_PASS_INITIAL_SYMBOLS);
    parse_tree_walk_run(walk, tree);

    check("initial-symbol pass visits each nonterminal on entry and exit",
          probe.enters > 0 && probe.enters == probe.exits);
    check("initial-symbol pass creates its module scope",
          core_tree_node_value(tree).scope != nullptr);
    check("initial-symbol pass records the declared name",
          parse_symtab_lookup_scoped(result->symtab,
              core_tree_node_value(tree).scope, S(""), S("a")) != nullptr);
    parse_sym_entry_t *inferred = parse_symtab_lookup_scoped(
        result->symtab, core_tree_node_value(tree).scope, S(""), S("a"));
    parse_annot_result_mark_inferred(result, inferred);
    parse_annot_result_mark_inferred(result, inferred);
    check("inferred-symbol worklist deduplicates symbol references",
          core_list_len(result->inferred) == 1);

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static void
test_apply_ordered_annotation_kinds(parse_grammar_t *g)
{
    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "prog", "vaT;{vbU;}?xyztcV;", &p, &ts);
    check("single-node annotation sample parses", tree != nullptr);
    if (!tree) return;

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    node_probe_t probe = {0};
    parse_annot_result_set_node_callback(result, count_annot_nodes, &probe);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register_pass(walk, result, nullptr, nullptr,
                              PARSE_ANNOT_PASS_INITIAL_SYMBOLS);
    const parse_annot_kind_t kinds[] = {PARSE_ANNOT_SCOPE_OPEN};

    parse_tree_walk_apply_node(walk, tree, nullptr, 0, -1, kinds,
                               sizeof(kinds) / sizeof(kinds[0]), true);
    check("single-node entry applies only the requested scope annotation",
          core_tree_node_value(tree).scope != nullptr && probe.enters == 1
              && probe.exits == 0);
    parse_tree_walk_apply_node(walk, tree, nullptr, 0, -1, kinds,
                               sizeof(kinds) / sizeof(kinds[0]), false);
    check("single-node exit balances the node callback without descending",
          probe.enters == 1 && probe.exits == 1);

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static void
apply_selected_tree(parse_tree_walk_t *walk, parse_tree_t *node,
                    parse_tree_t *parent, int32_t depth, int32_t child_index,
                    const parse_annot_kind_t *kinds, size_t kind_count)
{
    parse_tree_walk_apply_node(walk, node, parent, depth, child_index,
                               kinds, kind_count, true);
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        apply_selected_tree(walk, core_tree_child(node, i), node, depth + 1,
                            (int32_t)i, kinds, kind_count);
    }
    parse_tree_walk_apply_node(walk, node, parent, depth, child_index,
                               kinds, kind_count, false);
}

static void
test_selected_type_annotations_per_node(parse_grammar_t *g)
{
    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "prog", "tbI;", &p, &ts);
    check("per-node type annotation sample parses", tree != nullptr);
    if (!tree) return;

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register_pass(walk, result, types, nullptr,
                              PARSE_ANNOT_PASS_INITIAL_SYMBOLS);
    const parse_annot_kind_t kinds[] = {
        PARSE_ANNOT_SCOPE_OPEN,
        PARSE_ANNOT_DECLARES,
        PARSE_ANNOT_TYPE,
    };
    apply_selected_tree(walk, tree, nullptr, 0, -1, kinds,
                        sizeof(kinds) / sizeof(kinds[0]));

    parse_scope_t *scope = core_tree_node_value(tree).scope;
    parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(
        result->symtab, scope, S(""), S("b"));
    check("selected declaration and type annotations bind an explicitly typed symbol",
          symbol && is_type(symbol->type, "int"));
    check("per-node entry and exit preserve the module scope tree",
          scope && core_string_eq(scope->scope_tag, S("module"))
              && parse_symtab_current_scope(result->symtab, S(""))
                     == scope->parent);

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static void
test_inference_result_is_propagated(void)
{
    static const char *const G
        = "@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <decl>\n"
          "<decl> @declares($1, \"var\") @infer(\"$1 unify int\") "
          "::= \"v\" <name> \";\"\n"
          "<name> ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    check("inference propagation grammar loads", parse_bnf_load(S(G), S("top"), g));
    if (!parse_find_nonterm(g, S("top"))) {
        parse_grammar_free(g);
        return;
    }

    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "top", "va;", &p, &ts);
    check("inference propagation sample parses", tree != nullptr);
    if (!tree) {
        parse_grammar_free(g);
        return;
    }

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register_pass(walk, result, types, nullptr,
                              PARSE_ANNOT_PASS_INITIAL_SYMBOLS);
    const parse_annot_kind_t kinds[] = {
        PARSE_ANNOT_SCOPE_OPEN,
        PARSE_ANNOT_DECLARES,
        PARSE_ANNOT_INFER,
    };
    apply_selected_tree(walk, tree, nullptr, 0, -1, kinds,
                        sizeof(kinds) / sizeof(kinds[0]));

    parse_scope_t *scope = core_tree_node_value(tree).scope;
    parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(
        result->symtab, scope, S(""), S("a"));
    check("the infer annotation's unified result reaches its symbol",
          symbol && is_type(symbol->type, "int"));

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_symbol_lookup_instantiates_schemes(void)
{
    static const char *const G
        = "@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <expr>\n"
          "<expr> @declares($1, \"function\") @visibility(\"public\") "
          "::= \"v\" <name> \";\"\n"
          "<name> ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    check("scheme lookup grammar loads", parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "top", "va;", &p, &ts);
    check("scheme lookup sample parses", tree != nullptr);
    if (!tree) {
        parse_grammar_free(g);
        return;
    }

    th_ctx *tc = th_ctx_new();
    parse_annot_result_t *result = parse_annot_walk_tree(g, tree, tc, nullptr);
    parse_scope_t *scope = core_tree_node_value(tree).scope;
    parse_tree_t *expr_node = find_node(g, tree, "expr");
    th_type *variable = th_var(tc);
    th_param param = {.mode = TH_IN, .type = variable};
    th_type *identity = th_func(tc, variable, &param, 1, nullptr, nullptr, 0);
    parse_symtab_enter_scope(result->symtab, S(""), scope);
    parse_sym_entry_t *symbol = parse_symtab_lookup_in(scope, S("a"));
    symbol->type = th_generalize(tc, identity);
    parse_symtab_enter_scope(result->symtab, S(""), scope->parent);

    parse_infer_expr_t *lookup = parse_infer_parse(S("lookup($1)"));
    parse_visit_t visit = {
        .grammar = g,
        .node = expr_node,
        .pn = &core_tree_node_value(expr_node),
    };
    visit.pn->scope = scope;
    bool ok_a = false, ok_b = false;
    th_type *use_a = parse_infer_eval(lookup, &visit, tc, result->symtab,
                                     &ok_a);
    th_type *use_b = parse_infer_eval(lookup, &visit, tc, result->symtab,
                                     &ok_b);
    check("each lookup instantiates the exported scheme", ok_a && ok_b
              && use_a && use_b && !th_is_scheme(use_a)
              && !th_is_scheme(use_b) && use_a != use_b);
    check("per-node annotation clients retain declared visibility",
          core_string_eq(symbol->visibility, S("public")));

    parse_infer_free(lookup);
    parse_annot_result_free(result);
    th_ctx_free(tc);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_scoped_declaration_is_bound_outside_its_body(void)
{
    static const char *const G
        = "@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <func>\n"
          "<func> @scope(\"function\", $1) @declares($1, \"function\") "
          "::= \"f\" <name>\n"
          "<name> ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    check("scoped-declaration grammar loads", parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "top", "fa", &p, &ts);
    check("scoped-declaration sample parses", tree != nullptr);
    if (!tree) {
        parse_grammar_free(g);
        return;
    }

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register_pass(walk, result, nullptr, nullptr,
                              PARSE_ANNOT_PASS_INITIAL_SYMBOLS);
    parse_tree_walk_run(walk, tree);
    parse_tree_t *func = find_node(g, tree, "func");
    parse_sym_entry_t *symbol = func
        ? core_tree_node_value(func).sym : nullptr;

    check("function symbol is provisionally stored in its enclosing module",
          symbol && symbol->kind == PARSE_SYM_FUNCTION
              && symbol->scope == core_tree_node_value(tree).scope);
    check("function body still has its own scope",
          func && core_tree_node_value(func).scope
              && core_tree_node_value(func).scope
                     != core_tree_node_value(tree).scope);

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
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
        parse_grammar_t *bad = parse_grammar_new();

        if (parse_bnf_load(S(BAD[i]), S("top"), bad)) {
            printf("FAIL  this should not have loaded: %s", BAD[i]);
            fails++;
        }

        parse_grammar_free(bad);
    }
}

static void
test_pass_by_modes(void)
{
    static const char *const MODES[] = {"in", "out", "inout", "ref"};
    bool all_load = true;
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        core_buffer_t grammar_text = {0};
        core_buffer_printf(&grammar_text,
            "<top> @tokenizer(\"toy\") @scope(\"module\") ::= <p>\n"
            "<p> @declares($1,\"param\") @pass_by(\"%s\") ::= \"v\" <n> \";\"\n"
            "<n> ::= __LOWER\n", MODES[i]);
        parse_grammar_t *g = parse_grammar_new();
        parse_grammar_set_error_recovery(g, false);
        all_load = all_load && parse_bnf_load(
            core_string_borrow(grammar_text.data, grammar_text.byte_len),
            S("top"), g);
        parse_grammar_free(g);
        core_buffer_release(&grammar_text);
    }
    check("pass_by accepts in, out, inout, and ref values", all_load);
}

int
main(void)
{
    parse_grammar_t *g = load_toy();

    if (!g) {
        return 1;
    }

    test_annotation_arguments_only_need_balanced_parens();
    test_annotation_arguments_carry_integers();
    test_annotation_argument_lists_are_growable();
    test_the_representation_and_declaration_annotations();
    test_initial_symbol_walk_callback(g);
    test_apply_ordered_annotation_kinds(g);
    test_selected_type_annotations_per_node(g);
    test_inference_result_is_propagated();
    test_symbol_lookup_instantiates_schemes();
    test_scoped_declaration_is_bound_outside_its_body();
    test_the_repr_annotations_reach_the_layout_descriptor();
    test_an_adt_builds_a_variant_type();
    test_implements_is_checked_not_carried();
    test_part_two_rejections();
    test_pass_by_modes();
    test_annotations_are_retained_on_the_grammar(g);
    test_annotation_cache_is_thread_safe(g);
    test_the_walk_builds_a_symbol_table(g);
    test_a_deep_tree_does_not_overflow_the_stack(g);

    parse_grammar_free(g);

    return report("fe annot");
}
