#include "parse/grammar.h"
#include "parse/annotation.h"
#include "scanner/lex.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "private/parse/grammar.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t grammar_finalize_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
check_stack_for_nt(core_list_t(int) *stack, int64_t ruleset)
{
    for (size_t i = 0; i < stack->len; i++) {
        if (ruleset == (int64_t)stack->data[i]) {
            return true;
        }
    }

    return false;
}

static inline void
finalize_nullable(parse_nonterm_t *nt, bool value)
{
    nt->nullable  = value;
    nt->finalized = true;

    assert(!(nt->nullable && nt->group_nt) && "Group item cannot be nullable");
}

static bool
is_nullable_rule(parse_grammar_t *g, parse_rule_t *rule, core_list_t(int) *stack);

static bool
is_nullable_nt(parse_grammar_t *g, int64_t nt_id, core_list_t(int) *stack)
{
    if (stack && check_stack_for_nt(stack, nt_id)) {
        return true;
    }
    else {
        core_list_push(*stack, (int)nt_id);
    }

    parse_nonterm_t *nt = parse_get_nonterm(g, nt_id);

    if (nt->finalized) {
        return nt->nullable;
    }

    if (nt->rule_ids.len == 0) {
        finalize_nullable(nt, true);
        return true;
    }

    bool found_any_rule = false;

    for (size_t i = 0; i < nt->rule_ids.len; i++) {
        parse_rule_t *cur_rule = parse_get_rule(g, nt->rule_ids.data[i]);
        found_any_rule              = true;

        if (is_nullable_rule(g, cur_rule, stack)) {
            finalize_nullable(nt, true);
            (void)core_list_pop(int, *stack);
            return true;
        }
    }

    finalize_nullable(nt, !found_any_rule);
    (void)core_list_pop(int, *stack);

    return nt->nullable;
}

static bool
is_nullable_match(parse_grammar_t *g, parse_match_t *m, core_list_t(int) *stack)
{
    switch (m->kind) {
    case PARSE_MATCH_EMPTY:
        return true;
    case PARSE_MATCH_GROUP:
        return ((parse_rule_group_t *)m->group)->min == 0;
    case PARSE_MATCH_NT:
        return is_nullable_nt(g, m->nt_id, stack);
    default:
        return false;
    }
}

static bool
is_nullable_rule(parse_grammar_t *g, parse_rule_t *rule, core_list_t(int) *stack)
{
    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        parse_match_t *item = &rule->contents.data[i];

        if (!is_nullable_match(g, item, stack)) {
            return false;
        }

        if (item->kind == PARSE_MATCH_NT) {
            if (check_stack_for_nt(stack, item->nt_id)) {
                return false;
            }

            return is_nullable_nt(g, item->nt_id, stack);
        }
    }

    return false;
}

static bool
matches_eq(parse_match_t *a, parse_match_t *b)
{
    if (a->kind != b->kind) {
        return false;
    }

    switch (a->kind) {
    case PARSE_MATCH_EMPTY:
    case PARSE_MATCH_ANY:
        return true;
    case PARSE_MATCH_TERMINAL:
        return a->terminal_id == b->terminal_id;
    case PARSE_MATCH_CLASS:
        return a->char_class == b->char_class;
    case PARSE_MATCH_NT:
        return a->nt_id == b->nt_id;
    case PARSE_MATCH_GROUP:
        return a->group == b->group;
    case PARSE_MATCH_SET:
        return a->set_items == b->set_items;
    }

    return false;
}

bool parse_match_set_holds(const parse_match_set_t *s, int64_t tid)
{
    if (!s) {
        return false;
    }

    bool found = false;

    for (size_t i = 0; i < s->n && !found; i++) {
        found = s->ids[i] == tid;
    }

    return found != s->negate;
}

static bool
rule_exists(parse_grammar_t *g,
            parse_nonterm_t *nt,
            core_list_t(parse_match_t) *new_contents,
            int32_t *old_ix_out)
{
    size_t new_len = new_contents->len;

    for (size_t i = 0; i < nt->rule_ids.len; i++) {
        int32_t            rule_ix = nt->rule_ids.data[i];
        parse_rule_t *old     = parse_get_rule(g, rule_ix);

        if (old->contents.len != new_len) {
            continue;
        }

        bool match = true;

        for (size_t j = 0; j < new_len; j++) {
            if (!matches_eq(&old->contents.data[j], &new_contents->data[j])) {
                match = false;
                break;
            }
        }

        if (match) {
            if (old_ix_out) {
                *old_ix_out = rule_ix;
            }

            return true;
        }
    }

    return false;
}

static int32_t
add_rule_internal(parse_grammar_t *g,
                  parse_nt_id_t    nt_id,
                  core_list_t(parse_match_t) *items,
                  int      cost,
                  int32_t  penalty_link_ix,
                  int32_t *old_ix_out)
{
    assert(!g->finalized && "Cannot modify grammar after finalization");
    assert(items->len > 0 && "Empty productions not allowed");

    parse_nonterm_t *nt = parse_get_nonterm(g, nt_id);

    if (rule_exists(g, nt, items, old_ix_out)) {
        return -1;
    }

    parse_rule_t rule = {0};

    rule.nt_id        = nt_id;
    rule.contents     = *items;
    rule.cost         = cost;
    rule.penalty_rule = penalty_link_ix >= 0;
    rule.link_ix      = penalty_link_ix;

    core_list_push(g->rules, rule);

    int32_t ix = (int32_t)(g->rules.len - 1);
    core_list_push(nt->rule_ids, ix);

    return ix;
}

static void
create_one_error_rule_set(parse_grammar_t *g, int32_t rule_ix)
{
    parse_rule_t *cur = parse_get_rule(g, rule_ix);
    parse_nt_id_t cur_nt_id = cur->nt_id;
    parse_nonterm_t *cur_nt = parse_get_nonterm(g, cur_nt_id);

    if (cur_nt->no_error_rule) {
        return;
    }

    size_t n      = cur->contents.len;
    int    tok_ct = 0;

    for (size_t i = 0; i < n; i++) {
        parse_match_t *m = &cur->contents.data[i];

        if (m->kind == PARSE_MATCH_NT || m->kind == PARSE_MATCH_GROUP) {
            continue;
        }

        tok_ct++;

        parse_nonterm_t *cur_nt = parse_get_nonterm(g, cur_nt_id);

        core_buffer_t nb = {0};

        core_buffer_printf(&nb,
                           "$term-%s-%d-%d",
                           cur_nt->name.data ? cur_nt->name.data : "?",
                           rule_ix,
                           tok_ct);

        core_string_t name = core_string_from_raw(nb.data,
                                                (int64_t)nb.byte_len);

        core_buffer_release(&nb);

        parse_nonterm_t *nt_err    = parse_nonterm(g, name);
        parse_nt_id_t    nt_err_id = parse_nonterm_id(nt_err);

        core_list_t(parse_match_t) r1 = core_list_new(parse_match_t);
        core_list_push(r1, *m);
        int32_t ok_ix = add_rule_internal(g, nt_err_id, &r1, 0, -1, nullptr);

        core_list_t(parse_match_t) r2 = core_list_new(parse_match_t);
        parse_match_t empty           = {.kind = PARSE_MATCH_EMPTY};
        core_list_push(r2, empty);
        add_rule_internal(g, nt_err_id, &r2, 0, ok_ix, nullptr);

        core_list_t(parse_match_t) r3 = core_list_new(parse_match_t);
        parse_match_t any             = {.kind = PARSE_MATCH_ANY};
        core_list_push(r3, any);
        core_list_push(r3, *m);
        add_rule_internal(g, nt_err_id, &r3, 0, ok_ix, nullptr);

        cur                   = parse_get_rule(g, rule_ix);
        cur->contents.data[i] = (parse_match_t){.kind = PARSE_MATCH_NT, .nt_id = nt_err_id};
    }
}

static core_hash_value_t
name_key_hash(void *k)
{
    return core_string_hash(*(core_string_t *)k);
}

static bool
name_key_eq(const void *a, const void *b)
{
    return core_string_eq(*(const core_string_t *)a, *(const core_string_t *)b);
}

static core_string_t *
name_key(core_string_t s)
{
    core_string_t *k = core_alloc(core_string_t);

    *k = core_string_dup(s);

    return k;
}

parse_grammar_t *
parse_grammar_new(void)
{
    parse_grammar_t *g = core_alloc(parse_grammar_t);

    g->error_rules           = true;
    g->max_penalty           = PARSE_DEFAULT_MAX_PENALTY;
    g->hide_penalty_rewrites = true;
    g->hide_groups           = true;

    g->nt_map = core_alloc(core_dict_t);
    core_dict_init(g->nt_map, name_key_hash, name_key_eq);

    g->terminal_map = core_alloc(core_dict_t);
    core_dict_init(g->terminal_map, name_key_hash, name_key_eq);

    core_cache_init(&g->annot_cache);
    core_cache_set_enabled(&g->annot_cache, false);
    pthread_mutex_init(&g->annot_cache_lock, nullptr);

    static const core_string_t BUILTIN[] = {
        CORE_STRING_STATIC("IDENTIFIER"),
        CORE_STRING_STATIC("TYPEDEF_NAME"),
        CORE_STRING_STATIC("INTEGER"),
        CORE_STRING_STATIC("FLOAT"),
        CORE_STRING_STATIC("CHAR_LIT"),
        CORE_STRING_STATIC("STRING_LIT"),
    };

    for (size_t i = 0; i < sizeof(BUILTIN) / sizeof(BUILTIN[0]); i++) {
        parse_register_terminal(g, BUILTIN[i]);
    }

    return g;
}

static void
free_annotations(core_list_t(parse_annotation_ptr_t) * list)
{
    for (size_t i = 0; i < list->len; i++) {
        parse_annot_free(list->data[i]);
    }

    core_list_free(*list);
}

void
parse_grammar_free(parse_grammar_t *g)
{
    if (!g) {
        return;
    }

    core_cache_free(&g->annot_cache);
    pthread_mutex_destroy(&g->annot_cache_lock);

    for (size_t i = 0; i < g->rules.len; i++) {
        parse_rule_t *rule = &g->rules.data[i];

        core_list_free(rule->contents);
        free_annotations(&rule->annotations);
        core_free(rule->disambig_predicate.data);
        if (rule->first_set) {
            core_dict_free(rule->first_set);
            core_free(rule->first_set);
        }
    }

    core_list_free(g->rules);

    for (size_t i = 0; i < g->nt_list.len; i++) {
        parse_nonterm_t *nt = g->nt_list.data[i];

        core_list_free(nt->rule_ids);
        free_annotations(&nt->annotations);
        if (nt->first_set) {
            core_dict_free(nt->first_set);
            core_free(nt->first_set);
        }
        core_free(nt);
    }

    free_annotations(&g->annotations);

    core_list_free(g->nt_list);
    for (size_t i = 0; i < g->named_terms.len; i++) {
        core_free(g->named_terms.data[i].value.data);
    }

    core_list_free(g->named_terms);
    core_list_free(g->contextual_sources);

    if (g->lex_compiled) {
        scan_lex_free((scan_lex_t *)g->lex_compiled);
    }

    if (g->lex_spec_owned) {
        scan_lex_spec_free((const scan_lex_spec_t *)g->lex_spec);
    }

    if (g->nt_map) {
        core_dict_foreach (g->nt_map, e) {
            core_free(((core_string_t *)e->key)->data);
            core_free(e->key);
        }

        core_dict_free(g->nt_map);
        core_free(g->nt_map);
    }
    if (g->terminal_map) {
        core_dict_foreach (g->terminal_map, e) {
            core_free(((core_string_t *)e->key)->data);
            core_free(e->key);
        }

        core_dict_free(g->terminal_map);
        core_free(g->terminal_map);
    }

    if (g->has_terminal_categories) {
        for (size_t i = 0; i < g->terminal_categories.len; i++) {
            core_free(g->terminal_categories.data[i].data);
        }
        core_list_free(g->terminal_categories);
    }

    core_free(g);
}

void
parse_grammar_set_error_recovery(parse_grammar_t *g, bool enable)
{
    g->error_rules = enable;
}

void
parse_grammar_set_max_penalty(parse_grammar_t *g, uint32_t max)
{
    g->max_penalty = max;
}

parse_nonterm_t *
parse_find_nonterm(parse_grammar_t *g, core_string_t name)
{
    if (!g || !name.data) {
        return nullptr;
    }

    bool  found = false;
    void *val   = _core_dict_get(g->nt_map, &name, &found);

    return found ? parse_get_nonterm(g, (int64_t)(intptr_t)val) : nullptr;
}

parse_nonterm_t *
parse_nonterm(parse_grammar_t *g, core_string_t name)
{
    parse_nonterm_t *have = parse_find_nonterm(g, name);

    if (have) {
        return have;
    }

    parse_nonterm_t *nt = core_alloc(parse_nonterm_t);

    nt->id = (int64_t)g->nt_list.len;

    core_list_push(g->nt_list, nt);

    if (name.data) {
        core_string_t *key = name_key(name);

        nt->name = *key;
        _core_dict_put(g->nt_map, key, (void *)(intptr_t)nt->id);
    }

    return nt;
}

int32_t
parse_grammar_single_child_rule(parse_grammar_t *g, core_string_t parent_name,
                               core_string_t child_name)
{
    if (!g) return -1;
    parse_nonterm_t *parent = parse_find_nonterm(g, parent_name);
    parse_nonterm_t *child = parse_find_nonterm(g, child_name);
    if (!parent || !child) return -1;
    for (size_t i = 0; i < parent->rule_ids.len; i++) {
        parse_rule_t *rule = parse_get_rule(g, parent->rule_ids.data[i]);
        if (rule && rule->contents.len == 1
            && rule->contents.data[0].kind == PARSE_MATCH_NT
            && rule->contents.data[0].nt_id == child->id)
            return (int32_t)i;
    }
    return -1;
}

int64_t
parse_nonterm_id(parse_nonterm_t *nt)
{
    return nt->id;
}

int64_t
parse_lookup_terminal(parse_grammar_t *g, core_string_t name)
{
    if (!g || !name.data) {
        return 0;
    }

    bool  found = false;
    void *val   = _core_dict_get(g->terminal_map, &name, &found);

    return found ? (int64_t)(intptr_t)val : 0;
}

int64_t
parse_register_named_terminal(parse_grammar_t *g, core_string_t name)
{
    if (!g || !name.data || !name.u8_bytes) {
        return 0;
    }

    bool  found = false;
    void *val   = _core_dict_get(g->terminal_map, &name, &found);

    if (found) {
        return (int64_t)(intptr_t)val;
    }

    parse_terminal_t term = {0};

    term.value = core_string_dup(name);
    term.id    = (int64_t)g->named_terms.len + PARSE_TOK_START_ID + 1;

    core_list_push(g->named_terms, term);

    parse_terminal_t *stored = parse_get_terminal(g, term.id);
    _core_dict_put(g->terminal_map, name_key(stored->value),
                  (void *)(intptr_t)stored->id);

    return term.id;
}

int64_t
parse_register_terminal(parse_grammar_t *g, core_string_t name)
{
    if (!g || !name.data || !name.u8_bytes) {
        return 0;
    }

    int64_t existing = parse_lookup_terminal(g, name);

    if (existing) {
        return existing;
    }

    if (name.u8_bytes == 1) {
        int64_t id = (int64_t)(unsigned char)name.data[0];

        _core_dict_put(g->terminal_map, name_key(name),
                      (void *)(intptr_t)id);

        return id;
    }

    return parse_register_named_terminal(g, name);
}

void
parse_grammar_set_terminal_category(parse_grammar_t *g,
                                   int64_t         terminal_id,
                                   core_string_t   category)
{
    if (terminal_id < PARSE_TOK_START_ID) {
        return;
    }

    int32_t ix = (int32_t)(terminal_id - PARSE_TOK_START_ID);

    if (!g->has_terminal_categories) {
        g->terminal_categories     = core_list_new(core_string_t);
        g->has_terminal_categories = true;
    }

    while ((int32_t)g->terminal_categories.len <= ix) {
        core_list_push(g->terminal_categories, (core_string_t){0});
    }

    core_list_set(g->terminal_categories, ix, category);
}

void
parse_nonterm_set_user_data(parse_nonterm_t *nt, void *data)
{
    nt->user_data = data;
}

void *
parse_nonterm_get_user_data(parse_nonterm_t *nt)
{
    return nt->user_data;
}

void
parse_nonterm_set_action(parse_nonterm_t *nt, parse_walk_action_t action)
{
    if (nt) {
        nt->action = action;
    }
}

void
parse_grammar_set_default_action(parse_grammar_t *g, parse_walk_action_t action)
{
    if (g) {
        g->default_action = action;
    }
}

static _Atomic int32_t group_id_counter = 1;

parse_match_t
parse_group_match_v(parse_grammar_t *g, int min, int max, int n, parse_match_t *items)
{
    parse_rule_group_t *group = core_alloc(parse_rule_group_t);

    group->gid = atomic_fetch_add(&group_id_counter, 1);
    group->min = min;
    group->max = max;

    char namebuf[64];
    int nlen = snprintf(namebuf, sizeof(namebuf), "$$group_%d", group->gid);
    core_string_t name = core_string_borrow(namebuf,
                                             nlen > 0 ? (size_t)nlen : 0);

    parse_nonterm_t *nt = parse_nonterm(g, name);
    int64_t nt_id = parse_nonterm_id(nt);

    nt->group_nt = true;

    core_list_t(parse_match_t) match_items = core_list_new(parse_match_t);

    for (int i = 0; i < n; i++) {
        core_list_push(match_items, items[i]);
    }

    add_rule_internal(g, nt_id, &match_items, 0, -1, nullptr);

    group->contents_id = nt_id;

    return (parse_match_t){.kind = PARSE_MATCH_GROUP, .group = group};
}

parse_rule_t *
parse_add_rule_v(parse_grammar_t *g, parse_nt_id_t nt_id, int n, parse_match_t *items)
{
    core_list_t(parse_match_t) match_items = core_list_new(parse_match_t);

    for (int i = 0; i < n; i++) {
        core_list_push(match_items, items[i]);
    }

    int32_t old_ix = -1;
    int32_t ix     = add_rule_internal(g, nt_id, &match_items, 0, -1, &old_ix);

    return parse_get_rule(g, ix >= 0 ? ix : old_ix);
}

parse_rule_t *
parse_add_rule_with_cost_v(parse_grammar_t *g,
                          parse_nt_id_t    nt_id,
                          int             cost,
                          int             n,
                          parse_match_t   *items)
{
    core_list_t(parse_match_t) match_items = core_list_new(parse_match_t);

    for (int i = 0; i < n; i++) {
        core_list_push(match_items, items[i]);
    }

    int32_t old_ix = -1;
    int32_t ix     = add_rule_internal(g, nt_id, &match_items, cost, -1, &old_ix);

    return parse_get_rule(g, ix >= 0 ? ix : old_ix);
}

#define TERM_TO_PTR(id) ((void *)(uintptr_t)((uint64_t)(id) + 0x100))

static inline core_dict_t *
parse_dict_set_new(void)
{
    core_dict_t *d = core_alloc(core_dict_t);
    core_dict_init(d, nullptr, nullptr);
    return d;
}

static inline void
merge_dict_set_into(core_dict_t *dst, core_dict_t *src)
{
    if (!src) {
        return;
    }

    core_dict_foreach (src, e) {
        core_dict_add(dst, e->key, (void *)1);
    }
}

static inline bool
match_is_first_nullable(parse_grammar_t *g, parse_match_t *m, bool *first_nullable)
{
    (void)g;
    switch (m->kind) {
    case PARSE_MATCH_EMPTY:
        return true;
    case PARSE_MATCH_GROUP:
        return ((parse_rule_group_t *)m->group)->min == 0;
    case PARSE_MATCH_NT:
        return first_nullable[m->nt_id];
    default:
        return false;
    }
}

static bool
rule_is_first_nullable(parse_grammar_t *g, parse_rule_t *rule, bool *fn)
{
    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        if (!match_is_first_nullable(g, &rule->contents.data[i], fn)) {
            return false;
        }
    }

    return n > 0;
}

static bool
update_rule_first(parse_grammar_t *g, parse_rule_t *rule, bool *first_nullable)
{
    if (!rule->first_set) {
        rule->first_set = parse_dict_set_new();
    }

    size_t old_len      = rule->first_set->count;
    bool    old_has_any = rule->first_has_any;

    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        parse_match_t *m = &rule->contents.data[i];

        switch (m->kind) {
        case PARSE_MATCH_TERMINAL: {
            int32_t contextual
                = parse_contextual_source_tid(g, m->terminal_id);
            int64_t first_tid = contextual ? contextual : m->terminal_id;

            core_dict_add(rule->first_set, TERM_TO_PTR(first_tid), (void *)1);
            goto done;
        }
        case PARSE_MATCH_ANY:
        case PARSE_MATCH_CLASS:
        case PARSE_MATCH_SET:
            rule->first_has_any = true;
            goto done;
        case PARSE_MATCH_EMPTY:
            break;
        case PARSE_MATCH_GROUP: {
            parse_rule_group_t *grp = (parse_rule_group_t *)m->group;
            parse_nonterm_t    *gnt = parse_get_nonterm(g, grp->contents_id);

            if (gnt) {
                if (gnt->first_set) {
                    merge_dict_set_into(rule->first_set, gnt->first_set);
                }

                if (gnt->first_has_any) {
                    rule->first_has_any = true;
                }
            }

            if (grp->min > 0) {
                goto done;
            }

            break;
        }
        case PARSE_MATCH_NT: {
            parse_nonterm_t *nt = parse_get_nonterm(g, m->nt_id);

            if (nt) {
                if (nt->first_set) {
                    merge_dict_set_into(rule->first_set, nt->first_set);
                }

                if (nt->first_has_any) {
                    rule->first_has_any = true;
                }

                if (!first_nullable[m->nt_id]) {
                    goto done;
                }
            }
            else {
                rule->first_has_any = true;
                goto done;
            }

            break;
        }
        }
    }

done:
    return rule->first_set->count != old_len || rule->first_has_any != old_has_any;
}

static void
compute_first_nullable(parse_grammar_t *g, bool *first_nullable)
{
    size_t n_nts = g->nt_list.len;

    if (n_nts == 0) {
        return;
    }

    int32_t *wait_start = core_alloc_array(int32_t, n_nts + 1);
    int32_t *cursor     = core_alloc_array(int32_t, n_nts);

    for (size_t a = 0; a < n_nts; a++) {
        parse_nonterm_t *nt = parse_get_nonterm(g, (int64_t)a);

        for (size_t j = 0; j < nt->rule_ids.len; j++) {
            parse_rule_t *r = parse_get_rule(g, nt->rule_ids.data[j]);

            for (size_t k = 0; k < r->contents.len; k++) {
                if (r->contents.data[k].kind == PARSE_MATCH_NT) {
                    cursor[r->contents.data[k].nt_id]++;
                }
            }
        }
    }

    int32_t total = 0;

    for (size_t b = 0; b < n_nts; b++) {
        wait_start[b] = total;
        total += cursor[b];
        cursor[b]     = wait_start[b];
    }

    wait_start[n_nts] = total;

    int32_t *wait_list = core_alloc_array(int32_t, total ? total : 1);

    for (size_t a = 0; a < n_nts; a++) {
        parse_nonterm_t *nt = parse_get_nonterm(g, (int64_t)a);

        for (size_t j = 0; j < nt->rule_ids.len; j++) {
            parse_rule_t *r = parse_get_rule(g, nt->rule_ids.data[j]);

            for (size_t k = 0; k < r->contents.len; k++) {
                if (r->contents.data[k].kind == PARSE_MATCH_NT) {
                    wait_list[cursor[r->contents.data[k].nt_id]++] = (int32_t)a;
                }
            }
        }
    }

    core_list_t(int32_t) work = core_list_new_cap(int32_t, n_nts);

    for (size_t a = 0; a < n_nts; a++) {
        core_list_push(work, (int32_t)a);
    }

    while (core_list_len(work) > 0) {
        int32_t a = work.data[core_list_len(work) - 1];

        core_list_pop(int32_t, work);

        if (first_nullable[a]) {
            continue;
        }

        parse_nonterm_t *nt  = parse_get_nonterm(g, a);
        bool           now = false;

        for (size_t j = 0; j < nt->rule_ids.len && !now; j++) {
            parse_rule_t *r = parse_get_rule(g, nt->rule_ids.data[j]);

            now = rule_is_first_nullable(g, r, first_nullable);
        }

        if (!now) {
            continue;
        }

        first_nullable[a] = true;

        for (int32_t e = wait_start[a]; e < wait_start[a + 1]; e++) {
            if (!first_nullable[wait_list[e]]) {
                core_list_push(work, wait_list[e]);
            }
        }
    }

    core_list_free(work);
    core_free(wait_list);
    core_free(cursor);
    core_free(wait_start);
}

static void
first_deps_of_rule(parse_grammar_t       *g,
                   parse_rule_t    *rule,
                   const bool          *first_nullable,
                   core_list_t(int32_t) *out)
{
    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        parse_match_t *m = &rule->contents.data[i];

        switch (m->kind) {
        case PARSE_MATCH_TERMINAL:
        case PARSE_MATCH_ANY:
        case PARSE_MATCH_CLASS:
        case PARSE_MATCH_SET:
            return;

        case PARSE_MATCH_EMPTY:
            break;

        case PARSE_MATCH_GROUP: {
            parse_rule_group_t *grp = (parse_rule_group_t *)m->group;
            parse_nonterm_t    *gnt = parse_get_nonterm(g, grp->contents_id);

            if (gnt) {
                core_list_push(*out, (int32_t)grp->contents_id);
            }

            if (grp->min > 0) {
                return;
            }

            break;
        }

        case PARSE_MATCH_NT: {
            parse_nonterm_t *nt = parse_get_nonterm(g, m->nt_id);

            if (!nt) {
                return;
            }

            core_list_push(*out, (int32_t)m->nt_id);

            if (!first_nullable[m->nt_id]) {
                return;
            }

            break;
        }
        }
    }
}

static bool
refresh_nt_first(parse_grammar_t *g, parse_nonterm_t *nt)
{
    size_t old_len = nt->first_set->count;
    bool   old_any = nt->first_has_any;

    for (size_t j = 0; j < nt->rule_ids.len; j++) {
        parse_rule_t *rule = parse_get_rule(g, nt->rule_ids.data[j]);

        merge_dict_set_into(nt->first_set, rule->first_set);

        if (rule->first_has_any) {
            nt->first_has_any = true;
        }
    }

    return nt->first_set->count != old_len || nt->first_has_any != old_any;
}

typedef struct {
    core_list_t(int32_t) edges;
    int32_t            *start;
} first_deps_t;

static first_deps_t
build_first_deps(parse_grammar_t *g, bool *first_nullable)
{
    size_t       n_nts = g->nt_list.len;
    first_deps_t d     = {
        .edges = core_list_new_cap(int32_t, n_nts * 2),
        .start = core_alloc_array(int32_t, n_nts + 1),
    };

    for (size_t i = 0; i < n_nts; i++) {
        parse_nonterm_t *nt = parse_get_nonterm(g, (int64_t)i);

        d.start[i] = (int32_t)core_list_len(d.edges);

        for (size_t j = 0; j < nt->rule_ids.len; j++) {
            first_deps_of_rule(g,
                               parse_get_rule(g, nt->rule_ids.data[j]),
                               first_nullable,
                               &d.edges);
        }
    }

    d.start[n_nts] = (int32_t)core_list_len(d.edges);

    return d;
}

static void
settle_component(parse_grammar_t *g, core_list_t(int32_t) *members,
                 bool *first_nullable)
{
    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t k = 0; k < core_list_len(*members); k++) {
            int32_t        m  = members->data[k];
            parse_nonterm_t *nt = parse_get_nonterm(g, m);

            for (size_t j = 0; j < nt->rule_ids.len; j++) {
                parse_rule_t *r = parse_get_rule(g, nt->rule_ids.data[j]);

                if (update_rule_first(g, r, first_nullable)) {
                    changed = true;
                }
            }

            if (refresh_nt_first(g, nt)) {
                changed = true;
            }
        }
    }
}

static void
first_sets_by_scc(parse_grammar_t *g, bool *first_nullable)
{
    size_t n_nts = g->nt_list.len;

    if (n_nts == 0) {
        return;
    }

    first_deps_t deps     = build_first_deps(g, first_nullable);
    int32_t     *index    = core_alloc_array(int32_t, n_nts);
    int32_t     *low      = core_alloc_array(int32_t, n_nts);
    bool        *on_stack = core_alloc_array(bool, n_nts);

    for (size_t i = 0; i < n_nts; i++) {
        index[i] = -1;
    }

    core_list_t(int32_t) sccstack = core_list_new_cap(int32_t, n_nts);
    core_list_t(int32_t) frame_v  = core_list_new_cap(int32_t, n_nts);
    core_list_t(int32_t) frame_e  = core_list_new_cap(int32_t, n_nts);
    core_list_t(int32_t) members  = core_list_new_cap(int32_t, n_nts);

    int32_t next_index = 0;

    for (size_t root = 0; root < n_nts; root++) {
        if (index[root] >= 0) {
            continue;
        }

        core_list_push(frame_v, (int32_t)root);
        core_list_push(frame_e, deps.start[root]);

        index[root] = next_index;
        low[root]   = next_index;
        next_index++;
        on_stack[root] = true;
        core_list_push(sccstack, (int32_t)root);

        while (core_list_len(frame_v) > 0) {
            size_t  top = core_list_len(frame_v) - 1;
            int32_t v   = frame_v.data[top];
            int32_t e   = frame_e.data[top];

            if (e < deps.start[v + 1]) {
                frame_e.data[top] = e + 1;

                int32_t w = deps.edges.data[e];

                if (index[w] < 0) {
                    index[w] = next_index;
                    low[w]   = next_index;
                    next_index++;
                    on_stack[w] = true;
                    core_list_push(sccstack, w);
                    core_list_push(frame_v, w);
                    core_list_push(frame_e, deps.start[w]);
                }
                else if (on_stack[w] && low[v] > index[w]) {
                    low[v] = index[w];
                }

                continue;
            }

            core_list_pop(int32_t, frame_v);
            core_list_pop(int32_t, frame_e);

            if (core_list_len(frame_v) > 0) {
                size_t  parent = core_list_len(frame_v) - 1;
                int32_t pv     = frame_v.data[parent];

                if (low[pv] > low[v]) {
                    low[pv] = low[v];
                }
            }

            if (low[v] != index[v]) {
                continue;
            }

            core_list_clear(members);

            while (true) {
                int32_t m = sccstack.data[core_list_len(sccstack) - 1];

                core_list_pop(int32_t, sccstack);
                on_stack[m] = false;
                core_list_push(members, m);

                if (m == v) {
                    break;
                }
            }

            settle_component(g, &members, first_nullable);
        }
    }

    core_list_free(members);
    core_list_free(frame_e);
    core_list_free(frame_v);
    core_list_free(sccstack);
    core_free(on_stack);
    core_free(low);
    core_free(index);
    core_free(deps.start);
    core_list_free(deps.edges);
}

static void
compute_all_first_sets(parse_grammar_t *g)
{
    size_t n_nts   = g->nt_list.len;
    size_t n_rules = g->rules.len;

    bool *first_nullable = core_alloc_array(bool, n_nts);

    compute_first_nullable(g, first_nullable);

    for (size_t i = 0; i < n_nts; i++) {
        parse_nonterm_t *nt = parse_get_nonterm(g, (int64_t)i);

        nt->first_set     = parse_dict_set_new();
        nt->first_has_any = false;
    }

    for (size_t i = 0; i < n_rules; i++) {
        parse_rule_t *rule = parse_get_rule(g, (int32_t)i);

        rule->first_set     = parse_dict_set_new();
        rule->first_has_any = false;
    }

    first_sets_by_scc(g, first_nullable);

    for (size_t i = 0; i < n_nts; i++) {
        if (first_nullable[i]) {
            parse_get_nonterm(g, (int64_t)i)->first_has_any = true;
        }
    }

    core_free(first_nullable);
}

int64_t
parse_builtin_terminal_id(core_string_t name)
{
    static const struct {
        core_string_t lit;
        int64_t      id;
    } BUILTIN[] = {
        {CORE_STRING_STATIC("IDENTIFIER"),   PARSE_TOK_IDENTIFIER  },
        {CORE_STRING_STATIC("TYPEDEF_NAME"), PARSE_TOK_TYPEDEF_NAME},
        {CORE_STRING_STATIC("INTEGER"),      PARSE_TOK_INTEGER     },
        {CORE_STRING_STATIC("FLOAT"),        PARSE_TOK_FLOAT       },
        {CORE_STRING_STATIC("CHAR"),         PARSE_TOK_CHAR_LIT    },
        {CORE_STRING_STATIC("STRING"),       PARSE_TOK_STRING_LIT  },
    };

    for (size_t i = 0; i < sizeof(BUILTIN) / sizeof(BUILTIN[0]); i++) {
        if (core_string_eq(name, BUILTIN[i].lit)) {
            return BUILTIN[i].id;
        }
    }

    return 0;
}

int64_t
parse_terminal_id(parse_grammar_t *g, core_string_t name)
{
    int64_t id = parse_lookup_terminal(g, name);

    return id ? id : parse_builtin_terminal_id(name);
}

core_string_t
parse_terminal_name(const parse_grammar_t *g, int64_t tid)
{
    if (!g) {
        return (core_string_t){0};
    }

    for (size_t i = 0; i < core_list_len(g->named_terms); i++) {
        if (g->named_terms.data[i].id == tid) {
            return g->named_terms.data[i].value;
        }
    }

    return (core_string_t){0};
}

static bool
nt_is_undefined(const parse_nonterm_t *nt)
{
    if (!nt || nt->group_nt || !nt->name.data || !nt->name.u8_bytes) {
        return false;
    }

    if (nt->name.data[0] == '$') {
        return false;
    }

    return !nt->defined_here && !core_list_len(nt->rule_ids);
}

size_t
parse_grammar_undefined(const parse_grammar_t *g, core_string_t *out, size_t max)
{
    size_t found = 0;

    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        const parse_nonterm_t *nt = g->nt_list.data[i];

        if (!nt_is_undefined(nt)) {
            continue;
        }

        if (out && found < max) {
            out[found] = nt->name;
        }

        found++;
    }

    return found;
}

bool
parse_grammar_report_undefined(const parse_grammar_t *g, FILE *out)
{
    size_t n = parse_grammar_undefined(g, nullptr, 0);

    if (!n) {
        return true;
    }

    fprintf(out,
            "grammar: %zu nonterminal(s) are referenced but never defined:\n",
            n);

    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        const parse_nonterm_t *nt = g->nt_list.data[i];

        if (nt_is_undefined(nt)) {
            fprintf(out,
                    "     <%.*s>\n",
                    (int)nt->name.u8_bytes,
                    nt->name.data);
        }
    }

    return false;
}

void
parse_grammar_prepare_contextual(parse_grammar_t *g,
                                 const core_string_t *spellings,
                                 core_string_t source_name)
{
    if (!g) {
        return;
    }

    while (g->contextual_sources.len < g->named_terms.len) {
        core_list_push(g->contextual_sources, 0);
    }
    if (!spellings) {
        return;
    }

    if (!source_name.data) {
        source_name = CORE_STRING_STATIC("IDENTIFIER");
    }
    int32_t source_tid = (int32_t)parse_terminal_id(g, source_name);

    for (const core_string_t *word = spellings; word->data; word++) {
        int64_t terminal_id = parse_lookup_terminal(g, *word);
        int64_t ix = terminal_id - PARSE_TOK_START_ID - 1;

        if (ix >= 0 && (size_t)ix < g->contextual_sources.len) {
            g->contextual_sources.data[ix] = source_tid;
        }
    }
}

void
parse_grammar_finalize(parse_grammar_t *g)
{
    if (!g) {
        return;
    }

    pthread_mutex_lock(&grammar_finalize_lock);

    if (g->finalized) {
        pthread_mutex_unlock(&grammar_finalize_lock);
        return;
    }

    for (size_t i = 0; i < g->rules.len; i++) {
        parse_rule_t *rule = &g->rules.data[i];
        for (size_t j = 0; j < rule->annotations.len; j++) {
            if (rule->annotations.data[j]
                    && rule->annotations.data[j]->kind == PARSE_ANNOT_ADJACENT) {
                g->has_adjacency = true;
                break;
            }
        }
        if (g->has_adjacency) break;
    }

    const scan_lex_spec_t *lex_spec = scan_grammar_lex(g);

    parse_grammar_prepare_contextual(
        g, lex_spec ? lex_spec->soft_keywords : nullptr,
        lex_spec ? lex_spec->tok_ident : (core_string_t){0});

    size_t n = g->nt_list.len;

    for (size_t i = 0; i < n; i++) {
        core_list_t(int) stack = core_list_new(int);
        is_nullable_nt(g, (int64_t)i, &stack);
        core_list_free(stack);
    }

    if (g->error_rules) {
        n = g->rules.len;

        for (size_t i = 0; i < n; i++) {
            create_one_error_rule_set(g, (int32_t)i);
        }
    }

    compute_all_first_sets(g);

    (void)scan_grammar_lex_compiled(g);

    g->finalized = true;

    pthread_mutex_lock(&g->annot_cache_lock);
    core_cache_invalidate(&g->annot_cache);
    core_cache_set_enabled(&g->annot_cache, true);
    pthread_mutex_unlock(&g->annot_cache_lock);
    pthread_mutex_unlock(&grammar_finalize_lock);
}
