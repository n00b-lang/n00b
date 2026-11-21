#include "parse/annotation.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"

#include <pthread.h>

#include <string.h>

static const core_string_t kind_spellings[PARSE_ANNOT_KIND_COUNT] = {
#define PARSE_ANNOT_NAME(sym, spelling) \
    [PARSE_ANNOT_##sym] = CORE_STRING_STATIC(spelling),
    PARSE_ANNOT_KINDS(PARSE_ANNOT_NAME)
#undef PARSE_ANNOT_NAME
        [PARSE_ANNOT_NONE] = CORE_STRING_STATIC("none"),
};

parse_annotation_t *
parse_annot_new(parse_annot_kind_t kind)
{
    parse_annotation_t *a = core_alloc(parse_annotation_t);

    memset(a, 0, sizeof(*a));

    a->kind      = kind;
    a->name_ref  = -1;
    a->type_ref  = -1;
    a->value_ref = -1;

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

    c->args_text     = core_string_dup(a->args_text);
    c->source_origin = core_string_dup(a->source_origin);
    c->tag           = core_string_dup(a->tag);

    if (core_list_len(a->tags) > 0) {
        c->tags = core_list_new(core_string_t);
        for (size_t i = 0; i < core_list_len(a->tags); i++)
            core_list_push(c->tags, core_string_dup(a->tags.data[i]));
    }

    return c;
}

void
parse_annot_free(parse_annotation_t *a)
{
    if (!a) {
        return;
    }

    core_free(a->args_text.data);
    core_free(a->source_origin.data);
    core_free(a->tag.data);

    for (size_t i = 0; i < core_list_len(a->tags); i++)
        core_free(a->tags.data[i].data);
    core_list_free(a->tags);

    core_free(a);
}

parse_annot_kind_t
parse_annot_kind_from_name(core_string_t name)
{
    if (!name.data) {
        return PARSE_ANNOT_NONE;
    }

    for (int i = 0; i < PARSE_ANNOT_NONE; i++) {
        if (core_string_eq(kind_spellings[i], name)) {
            return (parse_annot_kind_t)i;
        }
    }

    return PARSE_ANNOT_NONE;
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

static parse_annotation_t *
first_of_kind(const core_list_t(parse_annotation_ptr_t) *list,
              parse_annot_kind_t kind)
{
    for (size_t i = 0; list && i < list->len; i++) {
        if (list->data[i] && list->data[i]->kind == kind) return list->data[i];
    }
    return nullptr;
}

static parse_annotation_t *
find_cached(parse_grammar_t *g, const void *owner, parse_query_t query,
            const core_list_t(parse_annotation_ptr_t) *list,
            parse_annot_kind_t kind)
{
    if (!g || !owner) return nullptr;

    void *hit = nullptr;
    pthread_mutex_lock(&g->annot_cache_lock);
    bool found = core_cache_get(&g->annot_cache, (void *)owner, query,
                                (uint64_t)kind, &hit);
    pthread_mutex_unlock(&g->annot_cache_lock);
    if (found) return hit;

    parse_annotation_t *out = first_of_kind(list, kind);
    pthread_mutex_lock(&g->annot_cache_lock);
    core_cache_put(&g->annot_cache, (void *)owner, query, (uint64_t)kind, out);
    pthread_mutex_unlock(&g->annot_cache_lock);
    return out;
}

parse_annotation_t *
parse_grammar_find_annot(parse_grammar_t *g, parse_annot_kind_t kind)
{
    return find_cached(g, g, PARSE_QUERY_GRAMMAR_ANNOT,
                       g ? &g->annotations : nullptr, kind);
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
    if (!g && rule) return first_of_kind(&rule->annotations, kind);
    return find_cached(g, rule, PARSE_QUERY_RULE_ANNOT,
                       rule ? &rule->annotations : nullptr, kind);
}

parse_annotation_t *
parse_nt_find_direct_annot(const parse_nonterm_t *nt, parse_annot_kind_t kind)
{
    return first_of_kind(nt ? &nt->annotations : nullptr, kind);
}

