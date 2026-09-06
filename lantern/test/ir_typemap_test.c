#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/typelang.h"

#include "lib/alloc.h"
#include "parse/annot_walk.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/symtab.h"
#include "scanner/token_stream.h"

static int fails = 0;

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static ncc_string_t S(const char *s)
{
    return ncc_string_from_cstr(s);
}

static ncc_string_t slurp(const char *path)
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

    return (ncc_string_t){.data = p, .u8_bytes = (int64_t)strlen(p)};
}


static th_type *want(th_ctx *th, const char *spec)
{
    char    *err = nullptr;
    th_type *t   = ir_typelang_parse(th, spec, &err);

    ncc_free(err);

    return t;
}

static bool same(th_ctx *th, th_type *a, const char *spec)
{
    th_type *b = want(th, spec);

    return a && b && th_hash(th, a) == th_hash(th, b);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char        path[1024];

    snprintf(path, sizeof(path), "%s/typemap.bnf", dir);

    ncc_string_t text = slurp(path);

    if (!text.data) {
        return 1;
    }

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    if (!ncc_bnf_load(text, S("prog"), g)) {
        check("typemap.bnf loads", false);

        return 1;
    }

    ncc_token_stream_t *ts = ncc_token_stream_from_codepoints(
        S("ia;sb;lc;de;{id;}"));
    ncc_pwz_parser_t   *p  = ncc_pwz_new(g);

    if (!ncc_pwz_parse(p, ts)) {
        check("the program parses", false);

        return 1;
    }

    ncc_parse_tree_t *tree = ncc_pwz_get_tree(p);

    check("the program parses to a tree", tree != nullptr);

    if (!tree) {
        return 1;
    }

    ir_ctx_t *ctx   = ir_ctx_new();
    th_ctx   *types = ir_ctx_types(ctx);

    ncc_annot_result_t *r = ncc_annot_walk_tree(g, tree, types);

    check("the annotation walk produces a result", r != nullptr);

    if (!r) {
        return 1;
    }

    size_t typed = 0;

    for (ncc_scope_t *s = r->symtab->all_scopes; s; s = s->all_next) {
        for (ncc_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->type) {
                typed++;
            }
        }
    }

    check("every declared type became a type, straight through typelang",
          typed == 5);

    ncc_scope_t *mod = nullptr;
    ncc_scope_t *blk = nullptr;

    for (ncc_scope_t *s = r->symtab->all_scopes; s; s = s->all_next) {
        if (s->scope_tag.data && !strcmp(s->scope_tag.data, "module")) {
            mod = s;
        }
        if (s->scope_tag.data && !strcmp(s->scope_tag.data, "block")) {
            blk = s;
        }
    }

    check("the walk opened a scope for each @scope annotation",
          mod != nullptr && blk != nullptr);
    check("and nested them as the grammar nests",
          blk && blk->parent == mod);

    ncc_sym_entry_t *a = ncc_symtab_lookup_scoped(r->symtab, mod,
                                                  ncc_string_empty(), S("a"));
    ncc_sym_entry_t *b = ncc_symtab_lookup_scoped(r->symtab, mod,
                                                  ncc_string_empty(), S("b"));
    ncc_sym_entry_t *c = ncc_symtab_lookup_scoped(r->symtab, mod,
                                                  ncc_string_empty(), S("c"));

    check("a module-level symbol is found", a != nullptr);
    check("so are its siblings", b != nullptr && c != nullptr);

    ncc_sym_entry_t *e = ncc_symtab_lookup_scoped(r->symtab, mod,
                                                  ncc_string_empty(), S("e"));

    if (a && b && c && e) {
        check("an unquoted type in the annotation becomes that type",
              same(types, a->type, "int"));
        check("a quoted type still becomes that type",
              same(types, b->type, "string"));
        check("a parameterized type becomes that type",
              same(types, c->type, "list[int]"));
        check("a comma inside the type's brackets is not an argument comma",
              same(types, e->type, "dict[int,string]"));
        check("and the two scalars do not collide",
              th_hash(types, a->type) != th_hash(types, b->type));
    }
    else {
        check("every declared symbol was found", false);
    }

    check("a symbol declared in an inner scope is not visible from the outer",
          ncc_symtab_lookup_scoped(r->symtab, mod, ncc_string_empty(),
                                   S("d"))
              == nullptr);

    ir_module_t *m = ir_module_new(ctx, "m", &ir_profile_bce, nullptr);

    ir_module_set_tree(m, g, tree);

    check("the module holds the parse tree it was built from",
          ir_module_tree(m) == tree && ir_module_grammar(m) == g);
    check("and a symbol table of its own",
          ir_module_symtab(m) != nullptr);
    check("and reaches the one type universe",
          ir_module_types(m) == types);

    ncc_parse_tree_t *decl = nullptr;

    for (ncc_scope_t *s = r->symtab->all_scopes; s && !decl; s = s->all_next) {
        for (ncc_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->decl_node) {
                decl = e->decl_node;
                break;
            }
        }
    }

    check("a symbol entry points back at the node that declared it",
          decl != nullptr);

    ncc_annot_result_free(r);
    ir_ctx_free(ctx);
    ncc_pwz_free(p);
    ncc_grammar_free(g);

    if (!fails) {
        printf("ir typemap: all checks passed\n");
    }

    return fails ? 1 : 0;
}
