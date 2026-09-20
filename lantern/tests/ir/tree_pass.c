#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/tree_pass.h"
#include "ir/typelang.h"
#include "ir/verify.h"

#include "lib/alloc.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "scanner/token_stream.h"
#include "harness.h"

static bool produced[IR_DIAG_COUNT];


static core_string_t
S(const char *s)
{
    return core_string_from_cstr(s);
}

static const char *const TOY
    = "@tokenizer(\"toy\")\n"
      "<prog> @scope(\"module\") ::= <items>\n"
      "<items> ::= <item> <items> | <item>\n"
      "<item> ::= <decl> | <cond> | <block> | <pair> | <num> | <rec> | <bad>\n"
      "<decl> @declares($1,\"var\") @type($1, int) ::= \"v\" <name> \";\"\n"
      "<lit> @infer(int) ::= \"n\"\n"
      "<num> @literal(\"int\") ::= \"q\"\n"
      "<rec> @scope(\"rec\") @record($1) ::= \"r\" <name> <fields>\n"
      "<fields> ::= <fld> <fields> | <fld>\n"
      "<fld> @field($1) @type($1, int) ::= \"f\" <name>\n"
      "<pair> @infer(list[$0]) ::= <lit> <lit>\n"
      "<bad> @infer(int unify string) ::= \"X\"\n"
      "<cond> @branch($1,$2,$3) ::= \"?\" <use> <use> <use>\n"
      "<use> @varref($0) ::= <name>\n"
      "<block> @scope(\"block\") ::= \"{\" <items> \"}\"\n"
      "<name> ::= __LOWER\n";

static parse_tree_t *
parse_toy(parse_grammar_t *g, const char *src, parse_pwz_parser_t **pp,
          parse_token_stream_t **tsp)
{
    *tsp = scan_token_stream_from_codepoints(S(src));
    *pp  = parse_pwz_new_for_start(
        g, parse_nonterm_id(parse_find_nonterm(g, S("prog"))));

    return parse_pwz_parse(*pp, *tsp) ? parse_pwz_get_tree(*pp) : nullptr;
}

static int hook_calls;

static bool
count_decls(parse_visit_t *v)
{
    (void)v;
    hook_calls++;

    return true;
}

static void
add_my_callback(parse_tree_walk_t *w, ir_module_t *m, void *state)
{
    (void)m;
    (void)state;

    parse_walk_on_nt(w, S("decl"), count_decls, nullptr, nullptr);
}

static parse_tree_t *
find_nt(parse_grammar_t *g, parse_tree_t *t, core_string_t want)
{
    if (!t || core_tree_is_leaf(t)) {
        return nullptr;
    }

    if (core_string_eq(parse_tree_nt_name(g, t), want)) {
        return t;
    }

    for (size_t i = 0; i < core_tree_num_children(t); i++) {
        parse_tree_t *hit = find_nt(g, core_tree_child(t, i), want);

        if (hit) {
            return hit;
        }
    }

    return nullptr;
}

static size_t
symbols_in(parse_symtab_t *st)
{
    size_t n = 0;

    for (parse_scope_t *s = st->all_scopes; s; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            n++;
        }
    }

    return n;
}

static size_t
scopes_in(parse_symtab_t *st)
{
    size_t n = 0;

    for (parse_scope_t *s = st->all_scopes; s; s = s->all_next) {
        n++;
    }

    return n;
}

int main(void)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    if (!parse_bnf_load(S(TOY), S("prog"), g)) {
        printf("FAIL  the toy grammar does not load\n");

        return 1;
    }

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    const char *toy_source = "va;vb;{va;}?xyznnqrpfmfnX";
    parse_tree_t   *tree = parse_toy(g, toy_source, &p,
                                         &ts);

    if (!tree) {
        printf("FAIL  the toy program does not parse\n");

        return 1;
    }

    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, nullptr);

    check("a module with no tree does not claim one",
          !(ir_module_props(m) & IR_PROP_TREE));

    ir_props_t unmet = 0;

    check("a tree pass will not run without a tree",
          ir_pass_run(&ir_pass_annot_scope, m, nullptr, &unmet, nullptr)
              == IR_PASS_UNMET);
    check("and says the tree is what is missing", unmet == IR_PROP_TREE);

    ir_module_set_tree(m, g, tree);

    check("setting the tree establishes it",
          ir_module_props(m) & IR_PROP_TREE);

    check("symbols will not run before scopes",
          ir_pass_run(&ir_pass_annot_symbols, m, nullptr, &unmet, nullptr)
              == IR_PASS_UNMET);
    check("and says scoping is what is missing", unmet == IR_PROP_SCOPED);

    check("the scope pass runs on its own",
          ir_tree_pass_run(&ir_pass_annot_scope, m, nullptr, nullptr, nullptr)
              == IR_PASS_OK);
    check("and establishes scoping", ir_module_props(m) & IR_PROP_SCOPED);

    parse_symtab_t *st = ir_module_symtab(m);

    check("the scope pass opened a scope per @scope annotation",
          scopes_in(st) == 4);
    check("and declared nothing", symbols_in(st) == 0);

    check("the symbols pass now runs",
          ir_tree_pass_run(&ir_pass_annot_symbols, m, add_my_callback, nullptr,
                           nullptr)
              == IR_PASS_OK);
    check("and establishes symbols", ir_module_props(m) & IR_PROP_SYMBOLS);

    check("a caller's own callback ran inside the pass", hook_calls == 3);

    check("it declared one symbol per @declares and @field",
          symbols_in(st) == 5);
    check("without opening the scopes a second time", scopes_in(st) == 4);

    parse_scope_t *mod = nullptr;

    for (parse_scope_t *s = st->all_scopes; s; s = s->all_next) {
        if (s->scope_tag.data && !strcmp(s->scope_tag.data, "module")) {
            mod = s;
        }
    }

    parse_sym_entry_t *a = mod ? parse_symtab_lookup_scoped(st, mod,
                                                        core_string_empty(),
                                                        S("a"))
                             : nullptr;

    check("a module-level symbol is there", a != nullptr);

    check("refine runs after symbols",
          ir_tree_pass_run(&ir_pass_annot_refine, m, nullptr, nullptr, nullptr)
              == IR_PASS_OK);
    check("and establishes tree types",
          ir_module_props(m) & IR_PROP_TREE_TYPED);

    if (a) {
        char    *err  = nullptr;
        th_type *want = ir_typelang_parse(ir_module_types(m), "int", &err);

        core_free(err);

        check("@type gave the symbol its type through typelang",
              a->type && want
                  && th_hash(ir_module_types(m), a->type)
                         == th_hash(ir_module_types(m), want));
    }

    check("control flow runs",
          ir_tree_pass_run(&ir_pass_annot_cflow, m, nullptr, nullptr, nullptr)
              == IR_PASS_OK);
    check("and establishes it", ir_module_props(m) & IR_PROP_CFLOW);

    check("inference will not run before symbols exist",
          ir_pass_run(&ir_pass_annot_infer, ir_module_new(ctx, "bare",
                                                          &ir_profile_bce,
                                                          nullptr),
                      nullptr, &unmet, nullptr)
              == IR_PASS_UNMET);

    check("the inference pass runs",
          ir_tree_pass_run(&ir_pass_annot_infer, m, nullptr, nullptr, nullptr)
              == IR_PASS_OK);

    ir_diags_t *inference_diags = ir_diags_new(ctx, nullptr);

    check("an inference mismatch is retained for diagnostics",
          ir_import_type_errors(m, inference_diags) == 1);

    const ir_diag_t *inference_diag = ir_diags_at(inference_diags, 0);
    uint32_t bad_col = (uint32_t)strlen(toy_source);

    check("an inference diagnostic points at its source production",
          inference_diag && inference_diag->primary.span.start_line == 1
              && inference_diag->primary.span.start_col == bad_col
              && inference_diag->primary.span.end_line == 1
              && inference_diag->primary.span.end_col == bad_col + 1);

    parse_tree_t *pair = find_nt(g, tree, CORE_STRING_STATIC("pair"));

    check("the inferred production was found", pair != nullptr);

    if (pair) {
        char    *err  = nullptr;
        th_type *want = ir_typelang_parse(ir_module_types(m), "list[int]",
                                          &err);

        core_free(err);

        th_type *got = core_tree_node_value(pair).type;

        check("@infer(list[$0]) built the type from its child's type",
              got && want
                  && th_hash(ir_module_types(m), got)
                         == th_hash(ir_module_types(m), want));
    }

    parse_tree_t *num = find_nt(g, tree, CORE_STRING_STATIC("num"));
    parse_tree_t *rec = find_nt(g, tree, CORE_STRING_STATIC("rec"));

    check("the @literal production was found", num != nullptr);
    check("the @record production was found", rec != nullptr);

    if (num) {
        char    *err  = nullptr;
        th_type *want = ir_typelang_parse(ir_module_types(m), "int", &err);

        core_free(err);

        th_type *got = core_tree_node_value(num).type;

        check("@literal types its node from the kind it names",
              got && want
                  && th_hash(ir_module_types(m), got)
                         == th_hash(ir_module_types(m), want));
    }

    if (rec) {
        th_type *got = core_tree_node_value(rec).type;

        check("@record built a record type", got != nullptr);
        check("with one member per @field", got && th_record_len(got) == 2);

        if (got && th_record_len(got) == 2) {
            const th_member *m0 = th_record_member(got, 0);
            const th_member *m1 = th_record_member(got, 1);

            check("and the members carry their @type", m0->type && m1->type);
        }

        parse_sym_entry_t *bound
            = parse_symtab_lookup_scoped(st, mod, core_string_empty(), S("p"));

        check("@record bound its type name as a symbol",
              bound != nullptr && bound->type != nullptr);
    }

    ir_diags_t *d = ir_diags_new(ctx, nullptr);

    check("the symbol check runs after control flow",
          ir_pass_run(&ir_pass_symcheck, m, nullptr, &unmet, d)
              == IR_PASS_OK);

    size_t unused    = 0;
    size_t shadowed  = 0;
    size_t undefined = 0;

    for (size_t i = 0; i < ir_diags_count(d); i++) {
        produced[ir_diags_at(d, (uint32_t)i)->code] = true;

        switch (ir_diags_at(d, (uint32_t)i)->code) {
        case IR_DIAG_UNUSED_SYMBOL:
            unused++;
            break;
        case IR_DIAG_SHADOWED_SYMBOL:
            shadowed++;
            break;
        case IR_DIAG_UNDEFINED_SYMBOL:
            undefined++;
            break;
        default:
            break;
        }
    }

    check("it reports the declared-and-never-used symbols", unused == 6);
    check("and reports every undefined use", undefined == 3);
    check("and reports the inner declaration that shadows an outer one",
          shadowed == 1);

    th_ctx  *tc = ir_module_types(m);
    th_type *i1 = ir_typelang_parse(tc, "int", nullptr);
    th_type *s1 = ir_typelang_parse(tc, "string", nullptr);

    th_errors_clear(tc);
    th_unify(tc, i1, s1);

    check("the unifier recorded the failure", th_error_count(tc) > 0);

    ir_diags_t *td = ir_diags_new(ctx, nullptr);

    check("the bridge turns every type error into a diagnostic",
          ir_import_type_errors(m, td) == 1);
    check("and clears them, so they are not reported twice",
          th_error_count(tc) == 0
              && ir_import_type_errors(m, td) == 0);
    check("the diagnostic is a type mismatch",
          ir_diags_count(td) == 1
              && ir_diags_at(td, 0)->code == IR_DIAG_TYPE_MISMATCH);
    check("and names both types",
          ir_diags_at(td, 0)->message
              && strstr(ir_diags_at(td, 0)->message, "int")
              && strstr(ir_diags_at(td, 0)->message, "string"));

    produced[IR_DIAG_TYPE_MISMATCH] = true;

    const ir_pass_t *const declared[] = {
        &ir_pass_annot_scope,
        &ir_pass_annot_symbols,
        &ir_pass_annot_refine,
        &ir_pass_annot_cflow,
        &ir_pass_annot_infer,
        &ir_pass_symcheck,
        &ir_pass_type_diags,
    };
    size_t n_declared = sizeof(declared) / sizeof(declared[0]);

    check("every pass the header declares is enumerable",
          ir_tree_pass_count() == n_declared);

    for (size_t i = 0; i < n_declared; i++) {
        bool found = false;

        for (size_t j = 0; j < ir_tree_pass_count(); j++) {
            found = found || ir_tree_pass_at(j) == declared[i];
        }

        check("and ir_tree_pass_at reaches it", found);
    }

    for (size_t i = 0; i < ir_tree_pass_count(); i++) {
        const ir_pass_t *tp = ir_tree_pass_at(i);

        check("and each one is named and runnable",
              tp && tp->name && tp->name[0] && tp->run);
    }

    ir_ctx_free(ctx);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);

    dump_produced_codes("produced-codes-tree.txt", produced);

    return report("ir tree passes");
}
