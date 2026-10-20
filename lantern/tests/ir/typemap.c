#include <stdio.h>
#include <stdlib.h>

#include "ir/ir.h"
#include "ir/typelang.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/annot_walk.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/symtab.h"
#include "scanner/token_stream.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static th_type *want(th_ctx *th, core_string_t spec)
{
    core_string_t err = {};
    th_type      *t   = ir_typelang_parse(th, spec, &err);

    core_free(err.data);

    return t;
}

static bool same(th_ctx *th, th_type *a, core_string_t spec)
{
    th_type *b = want(th, spec);

    return a && b && th_hash(th, a) == th_hash(th, b);
}

int main(int argc, char **argv)
{
    core_string_t  dir = argc > 1 ? core_string_from_cstr(argv[1])
                                  : core_string_dup(S("."));
    core_buffer_t *pb  = core_buffer_empty();

    core_buffer_printf(pb, "%.*s/typemap.bnf", (int)dir.u8_bytes, dir.data);
    core_free(dir.data);

    core_string_t paths = core_buffer_take_string(pb);
    char         *path  = core_string_to_cstr(paths);
    core_string_t text  = slurp(path);

    core_free(path);
    core_free(paths.data);

    if (!text.data) {
        return 1;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    if (!parse_bnf_load(text, S("prog"), g)) {
        check("typemap.bnf loads", false);

        return 1;
    }

    parse_token_stream_t *ts = scan_token_stream_from_codepoints(
        S("ia;sb;lc;de;{id;}"));
    parse_pwz_parser_t   *p  = parse_pwz_new_for_start(
        g, parse_nonterm_id(parse_find_nonterm(g, S("prog"))));

    if (!parse_pwz_parse(p, ts)) {
        check("the program parses", false);

        return 1;
    }

    parse_tree_t *tree = parse_pwz_get_tree(p);

    check("the program parses to a tree", tree != nullptr);

    if (!tree) {
        return 1;
    }

    ir_ctx_t *ctx   = ir_ctx_new();
    th_ctx   *types = ir_ctx_types(ctx);

    parse_annot_result_t *r = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, r);
    parse_annot_register(walk, r, types, nullptr);
    parse_tree_walk_run(walk, tree);
    parse_tree_walk_free(walk);

    check("the annotation walk produces a result", r != nullptr);

    if (!r) {
        return 1;
    }

    size_t typed = 0;

    for (parse_scope_t *s = r->symtab->all_scopes; s; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->type) {
                typed++;
            }
        }
    }

    check("every declared type became a type, straight through typelang",
          typed == 5);

    parse_scope_t *mod = nullptr;
    parse_scope_t *blk = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s; s = s->all_next) {
        if (s->scope_tag.data && core_string_eq(s->scope_tag, S("module"))) {
            mod = s;
        }
        if (s->scope_tag.data && core_string_eq(s->scope_tag, S("block"))) {
            blk = s;
        }
    }

    check("the walk opened a scope for each @scope annotation",
          mod != nullptr && blk != nullptr);
    check("and nested them as the grammar nests",
          blk && blk->parent == mod);

    parse_sym_entry_t *a = parse_symtab_lookup_scoped(r->symtab, mod,
                                                  core_string_empty(), S("a"));
    parse_sym_entry_t *b = parse_symtab_lookup_scoped(r->symtab, mod,
                                                  core_string_empty(), S("b"));
    parse_sym_entry_t *c = parse_symtab_lookup_scoped(r->symtab, mod,
                                                  core_string_empty(), S("c"));

    check("a module-level symbol is found", a != nullptr);
    check("so are its siblings", b != nullptr && c != nullptr);

    parse_sym_entry_t *e = parse_symtab_lookup_scoped(r->symtab, mod,
                                                  core_string_empty(), S("e"));

    if (a && b && c && e) {
        check("an unquoted type in the annotation becomes that type",
              same(types, a->type, S("int")));
        check("a quoted type still becomes that type",
              same(types, b->type, S("string")));
        check("a parameterized type becomes that type",
              same(types, c->type, S("list[int]")));
        check("a comma inside the type's brackets is not an argument comma",
              same(types, e->type, S("dict[int,string]")));
        check("and the two scalars do not collide",
              th_hash(types, a->type) != th_hash(types, b->type));
    }
    else {
        check("every declared symbol was found", false);
    }

    check("a symbol declared in an inner scope is not visible from the outer",
          parse_symtab_lookup_scoped(r->symtab, mod, core_string_empty(),
                                   S("d"))
              == nullptr);

    ir_module_t *m = ir_module_new(ctx, S("m"), &ir_profile_bce,
                                   (core_string_t){});

    ir_module_set_tree(m, g, tree);

    check("the module holds the parse tree it was built from",
          ir_module_tree(m) == tree && ir_module_grammar(m) == g);
    check("and a symbol table of its own",
          ir_module_symtab(m) != nullptr);
    check("and reaches the one type universe",
          ir_module_types(m) == types);

    ir_global_t *old_global = ir_global_add(m, S("old"), want(types, S("int")));
    ir_func_t *old_function = ir_func_add(m, S("old_function"),
        th_func(types, want(types, S("int")), nullptr, 0, nullptr, nullptr, 0),
        IR_LINK_INTERNAL);
    parse_sym_entry_t *old_symbol = parse_symtab_add(ir_module_symtab(m),
        S(""), S("old_symbol"), PARSE_SYM_VARIABLE, tree);
    ir_module_set_span(m, (ir_span_t){.file = 7});
    bool old_annotation = ir_annot_set(ctx, ir_module_annots(m),
        (ir_annot_t){.key = S("old.tree.note"), .kind = IR_ANNOT_BOOL,
                     .b = true});
    ir_module_establish(m, IR_PROP_SCOPED | IR_PROP_SYMBOLS
                              | IR_PROP_CFLOW | IR_PROP_TREE_TYPED);
    parse_tree_t *second_tree = core_tree_child(tree, 0);
    check("the replacement tree is distinct", second_tree != nullptr
                                          && second_tree != tree);
    ir_module_set_tree(m, g, second_tree);
    check("replacing a tree drops derived properties and IR",
          ir_module_props(m) == IR_PROP_TREE
              && ir_module_tree(m) == second_tree
              && ir_global_find(m, S("old")) == nullptr
              && ir_global_count(m) == 0
              && ir_func_find(m, S("old_function")) == nullptr
              && ir_func_count(m) == 0
              && ir_module_span(m).file == -1
              && ir_annot_get(ir_module_annots(m), S("old.tree.note"))
                     == nullptr
              && parse_symtab_lookup_scoped(ir_module_symtab(m), nullptr,
                  S(""), S("old_symbol")) == nullptr);
    check("the replacement can define the old name afresh",
          old_global && ir_global_add(m, S("old"), want(types, S("int")))
              != nullptr && old_function && old_symbol && old_annotation);
    bool replacement_annotation = ir_annot_set(ctx, ir_module_annots(m),
        (ir_annot_t){.key = S("replacement.tree.note"), .kind = IR_ANNOT_BOOL,
                     .b = true});
    ir_module_set_tree(m, g, nullptr);
    check("clearing a tree drops its IR and grammar",
          ir_module_props(m) == 0 && ir_module_tree(m) == nullptr
              && ir_module_grammar(m) == nullptr
              && ir_global_find(m, S("old")) == nullptr
              && replacement_annotation
              && ir_annot_get(ir_module_annots(m),
                              S("replacement.tree.note")) == nullptr);

    parse_tree_t *decl = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s && !decl; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->decl_node) {
                decl = e->decl_node;
                break;
            }
        }
    }

    check("a symbol entry points back at the node that declared it",
          decl != nullptr);

    parse_annot_result_free(r);
    ir_ctx_free(ctx);
    parse_pwz_free(p);
    parse_grammar_free(g);

    return report("ir typemap");
}
