#include "parse/annotation.h"
#include "parse/infer.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"

#include <string.h>

static const char *const kind_spellings[NCC_ANNOT_KIND_COUNT] = {
#define NCC_ANNOT_NAME(sym, spelling) [NCC_ANNOT_##sym] = spelling,
    NCC_ANNOT_KINDS(NCC_ANNOT_NAME)
#undef NCC_ANNOT_NAME
        [NCC_ANNOT_NONE] = "none",
};

ncc_annotation_t *
ncc_annot_new(ncc_annot_kind_t kind)
{
    ncc_annotation_t *a = ncc_alloc(ncc_annotation_t);

    memset(a, 0, sizeof(*a));

    a->kind            = kind;
    a->name_ref        = NCC_CHILD_NONE;
    a->type_ref        = NCC_CHILD_NONE;
    a->value_ref       = NCC_CHILD_NONE;
    a->notrivia_ref    = NCC_CHILD_NONE;
    a->adt_keyword_ref = NCC_CHILD_NONE;

    return a;
}

ncc_annotation_t *
ncc_annot_clone(const ncc_annotation_t *a)
{
    if (!a) {
        return nullptr;
    }

    ncc_annotation_t *c = ncc_alloc(ncc_annotation_t);

    *c = *a;

    c->infer_ast = ncc_infer_clone(a->infer_ast);

    c->args_text          = ncc_string_dup(a->args_text);
    c->scope_tag          = ncc_string_dup(a->scope_tag);
    c->type_spec          = ncc_string_dup(a->type_spec);
    c->infer_expr         = ncc_string_dup(a->infer_expr);
    c->adt_kind           = ncc_string_dup(a->adt_kind);
    c->visibility_spec    = ncc_string_dup(a->visibility_spec);
    c->op_kind            = ncc_string_dup(a->op_kind);
    c->sym_kind           = ncc_string_dup(a->sym_kind);
    c->disambig_predicate = ncc_string_dup(a->disambig_predicate);

    c->mutability           = ncc_string_dup(a->mutability);
    c->ns_name              = ncc_string_dup(a->ns_name);
    c->direction            = ncc_string_dup(a->direction);
    c->constraints          = ncc_string_dup(a->constraints);
    c->val_expr             = ncc_string_dup(a->val_expr);

    c->val_ref.name         = ncc_string_dup(a->val_ref.name);
    c->name_ref.name        = ncc_string_dup(a->name_ref.name);
    c->type_ref.name        = ncc_string_dup(a->type_ref.name);
    c->value_ref.name       = ncc_string_dup(a->value_ref.name);
    c->notrivia_ref.name    = ncc_string_dup(a->notrivia_ref.name);
    c->adt_keyword_ref.name = ncc_string_dup(a->adt_keyword_ref.name);

    return c;
}

void
ncc_annot_free(ncc_annotation_t *a)
{
    if (!a) {
        return;
    }

    ncc_infer_free(a->infer_ast);

    ncc_string_t owned[] = {
        a->args_text,
        a->mutability,
        a->ns_name,
        a->direction,
        a->constraints,
        a->val_expr,
        a->val_ref.name,
        a->scope_tag,
        a->type_spec,
        a->infer_expr,
        a->adt_kind,
        a->visibility_spec,
        a->op_kind,
        a->sym_kind,
        a->disambig_predicate,
        a->name_ref.name,
        a->type_ref.name,
        a->value_ref.name,
        a->notrivia_ref.name,
        a->adt_keyword_ref.name,
    };

    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); i++) {
        ncc_free(owned[i].data);
    }

    ncc_free(a);
}

const char *
ncc_annot_kind_name(ncc_annot_kind_t kind)
{
    if (kind < 0 || kind >= NCC_ANNOT_KIND_COUNT) {
        return "?";
    }

    return kind_spellings[kind];
}

ncc_annot_kind_t
ncc_annot_kind_from_name(ncc_string_t name)
{
    if (!name.data) {
        return NCC_ANNOT_NONE;
    }

    for (int i = 0; i < NCC_ANNOT_NONE; i++) {
        const char *s = kind_spellings[i];

        if (strlen(s) == name.u8_bytes
            && memcmp(s, name.data, name.u8_bytes) == 0) {
            return (ncc_annot_kind_t)i;
        }
    }

    return NCC_ANNOT_NONE;
}

void
ncc_nt_annotate(ncc_nonterm_t *nt, ncc_annotation_t *a)
{
    if (!nt || !a) {
        return;
    }

    ncc_list_push(nt->annotations, a);
}

void
ncc_rule_annotate(ncc_parse_rule_t *rule, ncc_annotation_t *a)
{
    if (!rule || !a) {
        return;
    }

    ncc_list_push(rule->annotations, a);
}

void
ncc_grammar_annotate(ncc_grammar_t *g, ncc_annotation_t *a)
{
    if (!g || !a) {
        return;
    }

    ncc_list_push(g->annotations, a);
}

size_t
ncc_grammar_annot_count(const ncc_grammar_t *g)
{
    return g ? g->annotations.len : 0;
}

ncc_annotation_t *
ncc_grammar_annot_at(const ncc_grammar_t *g, size_t i)
{
    if (!g || i >= g->annotations.len) {
        return nullptr;
    }

    return g->annotations.data[i];
}

ncc_annotation_t *
ncc_grammar_find_annot(const ncc_grammar_t *g, ncc_annot_kind_t kind)
{
    size_t n = ncc_grammar_annot_count(g);

    for (size_t i = 0; i < n; i++) {
        ncc_annotation_t *a = ncc_grammar_annot_at(g, i);

        if (a->kind == kind) {
            return a;
        }
    }

    return nullptr;
}

size_t
ncc_nt_annot_count(const ncc_nonterm_t *nt)
{
    return nt ? nt->annotations.len : 0;
}

ncc_annotation_t *
ncc_nt_annot_at(const ncc_nonterm_t *nt, size_t i)
{
    if (!nt || i >= nt->annotations.len) {
        return nullptr;
    }

    return nt->annotations.data[i];
}

size_t
ncc_rule_annot_count(const ncc_parse_rule_t *rule)
{
    return rule ? rule->annotations.len : 0;
}

ncc_annotation_t *
ncc_rule_annot_at(const ncc_parse_rule_t *rule, size_t i)
{
    if (!rule || i >= rule->annotations.len) {
        return nullptr;
    }

    return rule->annotations.data[i];
}

ncc_annotation_t *
ncc_rule_find_annot(const ncc_parse_rule_t *rule, ncc_annot_kind_t kind)
{
    for (size_t i = 0; rule && i < ncc_rule_annot_count(rule); i++) {
        ncc_annotation_t *a = ncc_rule_annot_at(rule, i);

        if (a->kind == kind) {
            return a;
        }
    }

    return nullptr;
}

ncc_annotation_t *
ncc_nt_find_annot(ncc_grammar_t       *g,
                  const ncc_nonterm_t *nt,
                  ncc_annot_kind_t     kind)
{
    size_t n = ncc_nt_annot_count(nt);

    for (size_t i = 0; i < n; i++) {
        ncc_annotation_t *a = ncc_nt_annot_at(nt, i);

        if (a->kind == kind) {
            return a;
        }
    }

    for (size_t i = 0; g && nt && i < ncc_list_len(nt->rule_ids); i++) {
        ncc_annotation_t *a
            = ncc_rule_find_annot(ncc_get_rule(g, nt->rule_ids.data[i]),
                                  kind);

        if (a) {
            return a;
        }
    }

    return nullptr;
}
