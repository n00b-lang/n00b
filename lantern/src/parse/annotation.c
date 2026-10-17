#include "parse/annotation.h"
#include "parse/infer.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"

#include <pthread.h>

#include <string.h>

static const char *const kind_spellings[PARSE_ANNOT_KIND_COUNT] = {
#define PARSE_ANNOT_NAME(sym, spelling) [PARSE_ANNOT_##sym] = spelling,
    PARSE_ANNOT_KINDS(PARSE_ANNOT_NAME)
#undef PARSE_ANNOT_NAME
        [PARSE_ANNOT_NONE] = "none",
};

parse_annotation_t *
parse_annot_new(parse_annot_kind_t kind)
{
    parse_annotation_t *a = core_alloc(parse_annotation_t);

    memset(a, 0, sizeof(*a));

    a->kind            = kind;
    a->name_ref        = PARSE_CHILD_NONE;
    a->type_ref        = PARSE_CHILD_NONE;
    a->value_ref       = PARSE_CHILD_NONE;
    a->notrivia_ref    = PARSE_CHILD_NONE;
    a->adt_keyword_ref = PARSE_CHILD_NONE;

    return a;
}

parse_annotation_t *
parse_annot_clone(const parse_annotation_t *a)
{
    if (!a) {
        return nullptr;
    }

    parse_annotation_t *c = core_alloc(parse_annotation_t);

    *c = *a;

    c->infer_ast = parse_infer_clone(a->infer_ast);

    c->args_text          = core_string_dup(a->args_text);
    c->source_origin      = core_string_dup(a->source_origin);
    c->scope_tag          = core_string_dup(a->scope_tag);
    c->type_spec          = core_string_dup(a->type_spec);
    c->infer_expr         = core_string_dup(a->infer_expr);
    c->adt_kind           = core_string_dup(a->adt_kind);
    c->visibility_spec    = core_string_dup(a->visibility_spec);
    c->op_kind            = core_string_dup(a->op_kind);
    c->sym_kind           = core_string_dup(a->sym_kind);
    c->disambig_predicate = core_string_dup(a->disambig_predicate);

    c->mutability           = core_string_dup(a->mutability);
    c->ns_name              = core_string_dup(a->ns_name);
    c->direction            = core_string_dup(a->direction);
    c->emit_action          = core_string_dup(a->emit_action);
    c->val_expr             = core_string_dup(a->val_expr);

    if (core_list_len(a->emit_operands) > 0) {
        c->emit_operands = core_list_new(parse_child_ref_t);
        for (size_t i = 0; i < core_list_len(a->emit_operands); i++) {
            parse_child_ref_t ref = a->emit_operands.data[i];
            ref.name = core_string_dup(ref.name);
            core_list_push(c->emit_operands, ref);
        }
    }

    if (core_list_len(a->constraints) > 0) {
        c->constraints = core_list_new(parse_con_spec_t);

        for (size_t i = 0; i < core_list_len(a->constraints); i++) {
            parse_con_spec_t s = a->constraints.data[i];
            parse_con_spec_t d = {
                .kind  = s.kind,
                .name  = core_string_dup(s.name),
                .types = core_list_new(core_string_t),
                .index = s.index,
            };

            for (size_t j = 0; j < core_list_len(s.types); j++) {
                core_list_push(d.types, core_string_dup(s.types.data[j]));
            }

            core_list_push(c->constraints, d);
        }
    }

    c->val_ref.name         = core_string_dup(a->val_ref.name);
    c->name_ref.name        = core_string_dup(a->name_ref.name);
    c->type_ref.name        = core_string_dup(a->type_ref.name);
    c->value_ref.name       = core_string_dup(a->value_ref.name);
    c->notrivia_ref.name    = core_string_dup(a->notrivia_ref.name);
    c->adt_keyword_ref.name = core_string_dup(a->adt_keyword_ref.name);

    return c;
}

void
parse_annot_free(parse_annotation_t *a)
{
    if (!a) {
        return;
    }

    parse_infer_free(a->infer_ast);

    core_string_t owned[] = {
        a->args_text,
        a->source_origin,
        a->mutability,
        a->ns_name,
        a->direction,
        a->emit_action,
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
        core_free(owned[i].data);
    }

    for (size_t i = 0; i < core_list_len(a->constraints); i++) {
        parse_con_spec_t *s = &a->constraints.data[i];

        for (size_t j = 0; j < core_list_len(s->types); j++) {
            core_free(s->types.data[j].data);
        }

        core_list_free(s->types);
        core_free(s->name.data);
    }

    core_list_free(a->constraints);

    for (size_t i = 0; i < core_list_len(a->emit_operands); i++)
        core_free(a->emit_operands.data[i].name.data);
    core_list_free(a->emit_operands);

    core_free(a);
}

const char *
parse_annot_kind_name(parse_annot_kind_t kind)
{
    if (kind < 0 || kind >= PARSE_ANNOT_KIND_COUNT) {
        return "?";
    }

    return kind_spellings[kind];
}

parse_annot_kind_t
parse_annot_kind_from_name(core_string_t name)
{
    if (!name.data) {
        return PARSE_ANNOT_NONE;
    }

    for (int i = 0; i < PARSE_ANNOT_NONE; i++) {
        const char *s = kind_spellings[i];

        if (strlen(s) == name.u8_bytes
            && memcmp(s, name.data, name.u8_bytes) == 0) {
            return (parse_annot_kind_t)i;
        }
    }

    return PARSE_ANNOT_NONE;
}

void
parse_nt_annotate(parse_nonterm_t *nt, parse_annotation_t *a)
{
    if (!nt || !a) {
        return;
    }

    core_list_push(nt->annotations, a);
}

void
parse_rule_annotate(parse_rule_t *rule, parse_annotation_t *a)
{
    if (!rule || !a) {
        return;
    }

    core_list_push(rule->annotations, a);
}

void
parse_grammar_annotate(parse_grammar_t *g, parse_annotation_t *a)
{
    if (!g || !a) {
        return;
    }

    core_list_push(g->annotations, a);
    pthread_mutex_lock(&g->annot_cache_lock);
    core_cache_invalidate(&g->annot_cache);
    pthread_mutex_unlock(&g->annot_cache_lock);
}

size_t
parse_grammar_annot_count(const parse_grammar_t *g)
{
    return g ? g->annotations.len : 0;
}

parse_annotation_t *
parse_grammar_annot_at(const parse_grammar_t *g, size_t i)
{
    if (!g || i >= g->annotations.len) {
        return nullptr;
    }

    return g->annotations.data[i];
}

parse_annotation_t *
parse_grammar_find_annot(parse_grammar_t *g, parse_annot_kind_t kind)
{
    void *hit = nullptr;

    if (g) {
        pthread_mutex_lock(&g->annot_cache_lock);
        bool found = core_cache_get(&g->annot_cache, g,
                                    PARSE_QUERY_GRAMMAR_ANNOT,
                                    (uint64_t)kind, &hit);
        pthread_mutex_unlock(&g->annot_cache_lock);
        if (found) {
            return (parse_annotation_t *)hit;
        }
    }

    size_t              n   = parse_grammar_annot_count(g);
    parse_annotation_t *out = nullptr;

    for (size_t i = 0; i < n && !out; i++) {
        parse_annotation_t *a = parse_grammar_annot_at(g, i);

        if (a->kind == kind) {
            out = a;
        }
    }

    if (g) {
        pthread_mutex_lock(&g->annot_cache_lock);
        core_cache_put(&g->annot_cache, g, PARSE_QUERY_GRAMMAR_ANNOT,
                       (uint64_t)kind, out);
        pthread_mutex_unlock(&g->annot_cache_lock);
    }

    return out;
}

bool
parse_grammar_declares_after_initializer(parse_grammar_t *g)
{
    parse_annotation_t *point = parse_grammar_find_annot(
        g, PARSE_ANNOT_DECLARATION_POINT);
    return point && core_string_eq(point->scope_tag,
                                   CORE_STRING_STATIC("after_initializer"));
}

size_t
parse_nt_annot_count(const parse_nonterm_t *nt)
{
    return nt ? nt->annotations.len : 0;
}

parse_annotation_t *
parse_nt_annot_at(const parse_nonterm_t *nt, size_t i)
{
    if (!nt || i >= nt->annotations.len) {
        return nullptr;
    }

    return nt->annotations.data[i];
}

size_t
parse_rule_annot_count(const parse_rule_t *rule)
{
    return rule ? rule->annotations.len : 0;
}

parse_annotation_t *
parse_rule_annot_at(const parse_rule_t *rule, size_t i)
{
    if (!rule || i >= rule->annotations.len) {
        return nullptr;
    }

    return rule->annotations.data[i];
}

parse_annotation_t *
parse_rule_find_annot(parse_grammar_t *g, const parse_rule_t *rule,
                    parse_annot_kind_t kind)
{
    void *hit = nullptr;

    if (g && rule) {
        pthread_mutex_lock(&g->annot_cache_lock);
        bool found = core_cache_get(&g->annot_cache, (void *)rule,
                                    PARSE_QUERY_RULE_ANNOT,
                                    (uint64_t)kind, &hit);
        pthread_mutex_unlock(&g->annot_cache_lock);
        if (found) {
            return (parse_annotation_t *)hit;
        }
    }

    parse_annotation_t *out = nullptr;

    for (size_t i = 0; rule && !out && i < parse_rule_annot_count(rule); i++) {
        parse_annotation_t *a = parse_rule_annot_at(rule, i);

        if (a->kind == kind) {
            out = a;
        }
    }

    if (g && rule) {
        pthread_mutex_lock(&g->annot_cache_lock);
        core_cache_put(&g->annot_cache, (void *)rule, PARSE_QUERY_RULE_ANNOT,
                       (uint64_t)kind, out);
        pthread_mutex_unlock(&g->annot_cache_lock);
    }

    return out;
}

parse_annotation_t *
parse_nt_find_direct_annot(const parse_nonterm_t *nt, parse_annot_kind_t kind)
{
    for (size_t i = 0; i < parse_nt_annot_count(nt); i++) {
        parse_annotation_t *annotation = parse_nt_annot_at(nt, i);
        if (annotation && annotation->kind == kind) return annotation;
    }
    return nullptr;
}
