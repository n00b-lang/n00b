#include "parse/annotation.h"
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

void
ncc_annot_free(ncc_annotation_t *a)
{
    if (!a) {
        return;
    }

    ncc_string_t owned[] = {
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
ncc_nt_find_annot(const ncc_nonterm_t *nt, ncc_annot_kind_t kind)
{
    size_t n = ncc_nt_annot_count(nt);

    for (size_t i = 0; i < n; i++) {
        ncc_annotation_t *a = ncc_nt_annot_at(nt, i);

        if (a->kind == kind) {
            return a;
        }
    }

    return nullptr;
}
