#include "parse/grammar.h"
#include "parse/annotation.h"
#include "lib/alloc.h"
#include "internal/parse/grammar_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
check_stack_for_nt(ncc_list_t(int) *stack, int64_t ruleset)
{
    for (size_t i = 0; i < stack->len; i++) {
        if (ruleset == (int64_t)stack->data[i]) {
            return true;
        }
    }

    return false;
}

static inline void
finalize_nullable(ncc_nonterm_t *nt, bool value)
{
    nt->nullable  = value;
    nt->finalized = true;

    assert(!(nt->nullable && nt->group_nt) && "Group item cannot be nullable");
}

static bool
is_nullable_rule(ncc_grammar_t *g, ncc_parse_rule_t *rule, ncc_list_t(int) *stack);

static bool
is_nullable_nt(ncc_grammar_t *g, int64_t nt_id, ncc_list_t(int) *stack)
{
    if (stack && check_stack_for_nt(stack, nt_id)) {
        return true;
    }
    else {
        ncc_list_push(*stack, (int)nt_id);
    }

    ncc_nonterm_t *nt = ncc_get_nonterm(g, nt_id);

    if (nt->finalized) {
        return nt->nullable;
    }

    if (nt->rule_ids.len == 0) {
        finalize_nullable(nt, true);
        return true;
    }

    bool found_any_rule = false;

    for (size_t i = 0; i < nt->rule_ids.len; i++) {
        ncc_parse_rule_t *cur_rule = ncc_get_rule(g, nt->rule_ids.data[i]);
        found_any_rule              = true;

        if (is_nullable_rule(g, cur_rule, stack)) {
            finalize_nullable(nt, true);
            (void)ncc_list_pop(int, *stack);
            return true;
        }
    }

    finalize_nullable(nt, !found_any_rule);
    (void)ncc_list_pop(int, *stack);

    return nt->nullable;
}

static bool
is_nullable_match(ncc_grammar_t *g, ncc_match_t *m, ncc_list_t(int) *stack)
{
    switch (m->kind) {
    case NCC_MATCH_EMPTY:
        return true;
    case NCC_MATCH_GROUP:
        return ((ncc_rule_group_t *)m->group)->min == 0;
    case NCC_MATCH_NT:
        return is_nullable_nt(g, m->nt_id, stack);
    default:
        return false;
    }
}

static bool
is_nullable_rule(ncc_grammar_t *g, ncc_parse_rule_t *rule, ncc_list_t(int) *stack)
{
    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        ncc_match_t *item = &rule->contents.data[i];

        if (!is_nullable_match(g, item, stack)) {
            return false;
        }

        if (item->kind == NCC_MATCH_NT) {
            if (check_stack_for_nt(stack, item->nt_id)) {
                return false;
            }

            return is_nullable_nt(g, item->nt_id, stack);
        }
    }

    return false;
}

static bool
matches_eq(ncc_match_t *a, ncc_match_t *b)
{
    if (a->kind != b->kind) {
        return false;
    }

    switch (a->kind) {
    case NCC_MATCH_EMPTY:
    case NCC_MATCH_ANY:
        return true;
    case NCC_MATCH_TERMINAL:
        return a->terminal_id == b->terminal_id;
    case NCC_MATCH_CLASS:
        return a->char_class == b->char_class;
    case NCC_MATCH_NT:
        return a->nt_id == b->nt_id;
    case NCC_MATCH_GROUP:
        return a->group == b->group;
    case NCC_MATCH_SET:
        return a->set_items == b->set_items;
    }

    return false;
}

bool ncc_match_set_holds(const ncc_match_set_t *s, int64_t tid)
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
rule_exists(ncc_grammar_t *g,
            ncc_nonterm_t *nt,
            ncc_list_t(ncc_match_t) *new_contents,
            int32_t *old_ix_out)
{
    size_t new_len = new_contents->len;

    for (size_t i = 0; i < nt->rule_ids.len; i++) {
        int32_t            rule_ix = nt->rule_ids.data[i];
        ncc_parse_rule_t *old     = ncc_get_rule(g, rule_ix);

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
add_rule_internal(ncc_grammar_t *g,
                  ncc_nt_id_t    nt_id,
                  ncc_list_t(ncc_match_t) *items,
                  int      cost,
                  int32_t  penalty_link_ix,
                  int32_t *old_ix_out)
{
    assert(!g->finalized && "Cannot modify grammar after finalization");
    assert(items->len > 0 && "Empty productions not allowed");

    ncc_nonterm_t *nt = ncc_get_nonterm(g, nt_id);

    if (rule_exists(g, nt, items, old_ix_out)) {
        return -1;
    }

    ncc_parse_rule_t rule = {0};

    rule.nt_id        = nt_id;
    rule.contents     = *items;
    rule.cost         = cost;
    rule.penalty_rule = penalty_link_ix >= 0;
    rule.link_ix      = penalty_link_ix;

    ncc_list_push(g->rules, rule);

    int32_t ix = (int32_t)(g->rules.len - 1);
    ncc_list_push(nt->rule_ids, ix);

    return ix;
}

static void
create_one_error_rule_set(ncc_grammar_t *g, int32_t rule_ix)
{
    ncc_parse_rule_t *cur = ncc_get_rule(g, rule_ix);
    ncc_nt_id_t cur_nt_id = cur->nt_id;
    ncc_nonterm_t *cur_nt = ncc_get_nonterm(g, cur_nt_id);

    if (cur_nt->no_error_rule) {
        return;
    }

    size_t n      = cur->contents.len;
    int    tok_ct = 0;

    for (size_t i = 0; i < n; i++) {
        ncc_match_t *m = &cur->contents.data[i];

        if (m->kind == NCC_MATCH_NT || m->kind == NCC_MATCH_GROUP) {
            continue;
        }

        tok_ct++;

        ncc_nonterm_t *cur_nt = ncc_get_nonterm(g, cur_nt_id);

        char namebuf[256];
        int nlen = snprintf(namebuf,
                            sizeof(namebuf),
                            "$term-%s-%d-%d",
                            cur_nt->name.data ? cur_nt->name.data : "?",
                            rule_ix,
                            tok_ct);
        ncc_string_t name = ncc_string_from_raw(namebuf,
                                                nlen > 0 ? nlen : 0);

        ncc_nonterm_t *nt_err    = ncc_nonterm(g, name);
        ncc_nt_id_t    nt_err_id = ncc_nonterm_id(nt_err);

        ncc_list_t(ncc_match_t) r1 = ncc_list_new(ncc_match_t);
        ncc_list_push(r1, *m);
        int32_t ok_ix = add_rule_internal(g, nt_err_id, &r1, 0, -1, nullptr);

        ncc_list_t(ncc_match_t) r2 = ncc_list_new(ncc_match_t);
        ncc_match_t empty           = {.kind = NCC_MATCH_EMPTY};
        ncc_list_push(r2, empty);
        add_rule_internal(g, nt_err_id, &r2, 0, ok_ix, nullptr);

        ncc_list_t(ncc_match_t) r3 = ncc_list_new(ncc_match_t);
        ncc_match_t any             = {.kind = NCC_MATCH_ANY};
        ncc_list_push(r3, any);
        ncc_list_push(r3, *m);
        add_rule_internal(g, nt_err_id, &r3, 0, ok_ix, nullptr);

        cur                   = ncc_get_rule(g, rule_ix);
        cur->contents.data[i] = (ncc_match_t){.kind = NCC_MATCH_NT, .nt_id = nt_err_id};
    }
}

ncc_grammar_t *
ncc_grammar_new(void)
{
    ncc_grammar_t *g = ncc_alloc(ncc_grammar_t);

    g->error_rules           = true;
    g->max_penalty           = NCC_DEFAULT_MAX_PENALTY;
    g->hide_penalty_rewrites = true;
    g->hide_groups           = true;

    g->nt_map = ncc_alloc(ncc_dict_t);
    ncc_dict_init(g->nt_map, ncc_hash_cstring, ncc_dict_cstr_eq);

    g->terminal_map = ncc_alloc(ncc_dict_t);
    ncc_dict_init(g->terminal_map, ncc_hash_cstring, ncc_dict_cstr_eq);

    static const ncc_string_t BUILTIN[] = {
        NCC_STRING_STATIC("IDENTIFIER"),
        NCC_STRING_STATIC("TYPEDEF_NAME"),
        NCC_STRING_STATIC("INTEGER"),
        NCC_STRING_STATIC("FLOAT"),
        NCC_STRING_STATIC("CHAR_LIT"),
        NCC_STRING_STATIC("STRING_LIT"),
    };

    for (size_t i = 0; i < sizeof(BUILTIN) / sizeof(BUILTIN[0]); i++) {
        ncc_register_terminal(g, BUILTIN[i]);
    }

    return g;
}

static void
free_annotations(ncc_list_t(ncc_annotation_ptr_t) * list)
{
    for (size_t i = 0; i < list->len; i++) {
        ncc_annot_free(list->data[i]);
    }

    ncc_list_free(*list);
}

void
ncc_grammar_free(ncc_grammar_t *g)
{
    if (!g) {
        return;
    }

    for (size_t i = 0; i < g->rules.len; i++) {
        ncc_list_free(g->rules.data[i].contents);
        free_annotations(&g->rules.data[i].annotations);
    }

    ncc_list_free(g->rules);

    for (size_t i = 0; i < g->nt_list.len; i++) {
        ncc_nonterm_t *nt = g->nt_list.data[i];

        ncc_list_free(nt->rule_ids);
        free_annotations(&nt->annotations);
        ncc_free(nt);
    }

    free_annotations(&g->annotations);

    ncc_list_free(g->nt_list);
    ncc_list_free(g->named_terms);

    if (g->nt_map) {
        ncc_free(g->nt_map);
    }
    if (g->terminal_map) {
        ncc_free(g->terminal_map);
    }

    if (g->has_terminal_categories) {
        for (size_t i = 0; i < g->terminal_categories.len; i++) {
            ncc_free(g->terminal_categories.data[i].data);
        }
        ncc_list_free(g->terminal_categories);
    }

    ncc_free(g);
}

void
ncc_grammar_set_start_id(ncc_grammar_t *g, ncc_nt_id_t nt_id)
{
    if (g->default_start == nt_id) {
        return;
    }

    ncc_nonterm_t *was = ncc_get_nonterm(g, g->default_start);

    if (was) {
        was->start_nt = false;
    }

    g->default_start = nt_id;

    ncc_nonterm_t *now = ncc_get_nonterm(g, nt_id);

    if (now) {
        now->start_nt = true;
    }
}

void
ncc_grammar_set_error_recovery(ncc_grammar_t *g, bool enable)
{
    g->error_rules = enable;
}

void
ncc_grammar_set_max_penalty(ncc_grammar_t *g, uint32_t max)
{
    g->max_penalty = max;
}

ncc_nonterm_t *
ncc_find_nonterm(ncc_grammar_t *g, ncc_string_t name)
{
    if (!g || !name.data) {
        return nullptr;
    }

    bool  found = false;
    void *val   = _ncc_dict_get(g->nt_map, (void *)name.data, &found);

    return found ? ncc_get_nonterm(g, (int64_t)(intptr_t)val) : nullptr;
}

ncc_nonterm_t *
ncc_nonterm(ncc_grammar_t *g, ncc_string_t name)
{
    ncc_nonterm_t *have = ncc_find_nonterm(g, name);

    if (have) {
        return have;
    }

    ncc_nonterm_t *nt = ncc_alloc(ncc_nonterm_t);

    nt->name = name;
    nt->id   = (int64_t)g->nt_list.len;

    ncc_list_push(g->nt_list, nt);

    if (name.data) {
        _ncc_dict_put(g->nt_map, (void *)nt->name.data,
                      (void *)(intptr_t)nt->id);
    }

    return nt;
}

int64_t
ncc_nonterm_id(ncc_nonterm_t *nt)
{
    return nt->id;
}

int64_t
ncc_lookup_terminal(ncc_grammar_t *g, ncc_string_t name)
{
    if (!g || !name.data) {
        return 0;
    }

    bool  found = false;
    void *val   = _ncc_dict_get(g->terminal_map, (void *)name.data, &found);

    return found ? (int64_t)(intptr_t)val : 0;
}

int64_t
ncc_register_terminal(ncc_grammar_t *g, ncc_string_t name)
{
    bool  found = false;
    void *val   = _ncc_dict_get(g->terminal_map, (void *)name.data, &found);

    if (found) {
        return (int64_t)(intptr_t)val;
    }

    if (name.u8_bytes == 1) {
        int64_t id = (int64_t)(unsigned char)name.data[0];

        _ncc_dict_put(g->terminal_map, (void *)name.data,
                      (void *)(intptr_t)id);

        return id;
    }

    ncc_terminal_t term = {0};

    term.value = name;
    term.id    = (int64_t)g->named_terms.len + NCC_TOK_START_ID + 1;

    ncc_list_push(g->named_terms, term);

    ncc_terminal_t *stored = ncc_get_terminal(g, term.id);
    _ncc_dict_put(g->terminal_map,
                           (void *)stored->value.data,
                           (void *)(intptr_t)stored->id);

    return term.id;
}

void
ncc_grammar_set_terminal_category(ncc_grammar_t *g,
                                   int64_t         terminal_id,
                                   ncc_string_t   category)
{
    if (terminal_id < NCC_TOK_START_ID) {
        return;
    }

    int32_t ix = (int32_t)(terminal_id - NCC_TOK_START_ID);

    if (!g->has_terminal_categories) {
        g->terminal_categories     = ncc_list_new(ncc_string_t);
        g->has_terminal_categories = true;
    }

    while ((int32_t)g->terminal_categories.len <= ix) {
        ncc_list_push(g->terminal_categories, (ncc_string_t){0});
    }

    ncc_list_set(g->terminal_categories, ix, category);
}

void
ncc_nonterm_set_user_data(ncc_nonterm_t *nt, void *data)
{
    nt->user_data = data;
}

void *
ncc_nonterm_get_user_data(ncc_nonterm_t *nt)
{
    return nt->user_data;
}

void
ncc_nonterm_set_action(ncc_nonterm_t *nt, ncc_walk_action_t action)
{
    if (nt) {
        nt->action = action;
    }
}

void
ncc_grammar_set_default_action(ncc_grammar_t *g, ncc_walk_action_t action)
{
    if (g) {
        g->default_action = action;
    }
}

static int32_t group_id_counter = 1;

ncc_match_t
ncc_group_match_v(ncc_grammar_t *g, int min, int max, int n, ncc_match_t *items)
{
    ncc_rule_group_t *group = ncc_alloc(ncc_rule_group_t);

    group->gid = group_id_counter++;
    group->min = min;
    group->max = max;

    char namebuf[64];
    int nlen = snprintf(namebuf, sizeof(namebuf), "$$group_%d", group->gid);
    ncc_string_t name = ncc_string_from_raw(namebuf, nlen > 0 ? nlen : 0);

    ncc_nonterm_t *nt = ncc_nonterm(g, name);
    int64_t nt_id = ncc_nonterm_id(nt);

    nt->group_nt = true;

    ncc_list_t(ncc_match_t) match_items = ncc_list_new(ncc_match_t);

    for (int i = 0; i < n; i++) {
        ncc_list_push(match_items, items[i]);
    }

    add_rule_internal(g, nt_id, &match_items, 0, -1, nullptr);

    group->contents_id = nt_id;

    return (ncc_match_t){.kind = NCC_MATCH_GROUP, .group = group};
}

ncc_parse_rule_t *
ncc_add_rule_v(ncc_grammar_t *g, ncc_nt_id_t nt_id, int n, ncc_match_t *items)
{
    ncc_list_t(ncc_match_t) match_items = ncc_list_new(ncc_match_t);

    for (int i = 0; i < n; i++) {
        ncc_list_push(match_items, items[i]);
    }

    int32_t old_ix = -1;
    int32_t ix     = add_rule_internal(g, nt_id, &match_items, 0, -1, &old_ix);

    return ncc_get_rule(g, ix >= 0 ? ix : old_ix);
}

ncc_parse_rule_t *
ncc_add_rule_with_cost_v(ncc_grammar_t *g,
                          ncc_nt_id_t    nt_id,
                          int             cost,
                          int             n,
                          ncc_match_t   *items)
{
    ncc_list_t(ncc_match_t) match_items = ncc_list_new(ncc_match_t);

    for (int i = 0; i < n; i++) {
        ncc_list_push(match_items, items[i]);
    }

    int32_t old_ix = -1;
    int32_t ix     = add_rule_internal(g, nt_id, &match_items, cost, -1, &old_ix);

    return ncc_get_rule(g, ix >= 0 ? ix : old_ix);
}

#define TERM_TO_PTR(id) ((void *)(uintptr_t)((uint64_t)(id) + 0x100))

static inline ncc_dict_t *
ncc_dict_set_new(void)
{
    ncc_dict_t *d = ncc_alloc(ncc_dict_t);
    ncc_dict_init(d, nullptr, nullptr);
    return d;
}

static inline void
merge_dict_set_into(ncc_dict_t *dst, ncc_dict_t *src)
{
    if (!src) {
        return;
    }

    ncc_dict_foreach (src, e) {
        ncc_dict_add(dst, e->key, (void *)1);
    }
}

static inline bool
match_is_first_nullable(ncc_grammar_t *g, ncc_match_t *m, bool *first_nullable)
{
    (void)g;
    switch (m->kind) {
    case NCC_MATCH_EMPTY:
        return true;
    case NCC_MATCH_GROUP:
        return ((ncc_rule_group_t *)m->group)->min == 0;
    case NCC_MATCH_NT:
        return first_nullable[m->nt_id];
    default:
        return false;
    }
}

static bool
rule_is_first_nullable(ncc_grammar_t *g, ncc_parse_rule_t *rule, bool *fn)
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
update_rule_first(ncc_grammar_t *g, ncc_parse_rule_t *rule, bool *first_nullable)
{
    if (!rule->first_set) {
        rule->first_set = ncc_dict_set_new();
    }

    size_t old_len      = rule->first_set->count;
    bool    old_has_any = rule->first_has_any;

    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        ncc_match_t *m = &rule->contents.data[i];

        switch (m->kind) {
        case NCC_MATCH_TERMINAL:
            ncc_dict_add(rule->first_set, TERM_TO_PTR(m->terminal_id), (void *)1);
            goto done;
        case NCC_MATCH_ANY:
        case NCC_MATCH_CLASS:
        case NCC_MATCH_SET:
            rule->first_has_any = true;
            goto done;
        case NCC_MATCH_EMPTY:
            break;
        case NCC_MATCH_GROUP: {
            ncc_rule_group_t *grp = (ncc_rule_group_t *)m->group;
            ncc_nonterm_t    *gnt = ncc_get_nonterm(g, grp->contents_id);

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
        case NCC_MATCH_NT: {
            ncc_nonterm_t *nt = ncc_get_nonterm(g, m->nt_id);

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
compute_first_nullable(ncc_grammar_t *g, bool *first_nullable)
{
    size_t n_nts = g->nt_list.len;

    if (n_nts == 0) {
        return;
    }

    int32_t *wait_start = ncc_alloc_array(int32_t, n_nts + 1);
    int32_t *cursor     = ncc_alloc_array(int32_t, n_nts);

    for (size_t a = 0; a < n_nts; a++) {
        ncc_nonterm_t *nt = ncc_get_nonterm(g, (int64_t)a);

        for (size_t j = 0; j < nt->rule_ids.len; j++) {
            ncc_parse_rule_t *r = ncc_get_rule(g, nt->rule_ids.data[j]);

            for (size_t k = 0; k < r->contents.len; k++) {
                if (r->contents.data[k].kind == NCC_MATCH_NT) {
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

    int32_t *wait_list = ncc_alloc_array(int32_t, total ? total : 1);

    for (size_t a = 0; a < n_nts; a++) {
        ncc_nonterm_t *nt = ncc_get_nonterm(g, (int64_t)a);

        for (size_t j = 0; j < nt->rule_ids.len; j++) {
            ncc_parse_rule_t *r = ncc_get_rule(g, nt->rule_ids.data[j]);

            for (size_t k = 0; k < r->contents.len; k++) {
                if (r->contents.data[k].kind == NCC_MATCH_NT) {
                    wait_list[cursor[r->contents.data[k].nt_id]++] = (int32_t)a;
                }
            }
        }
    }

    ncc_list_t(int32_t) work = ncc_list_new_cap(int32_t, n_nts);

    for (size_t a = 0; a < n_nts; a++) {
        ncc_list_push(work, (int32_t)a);
    }

    while (ncc_list_len(work) > 0) {
        int32_t a = work.data[ncc_list_len(work) - 1];

        ncc_list_pop(int32_t, work);

        if (first_nullable[a]) {
            continue;
        }

        ncc_nonterm_t *nt  = ncc_get_nonterm(g, a);
        bool           now = false;

        for (size_t j = 0; j < nt->rule_ids.len && !now; j++) {
            ncc_parse_rule_t *r = ncc_get_rule(g, nt->rule_ids.data[j]);

            now = rule_is_first_nullable(g, r, first_nullable);
        }

        if (!now) {
            continue;
        }

        first_nullable[a] = true;

        for (int32_t e = wait_start[a]; e < wait_start[a + 1]; e++) {
            if (!first_nullable[wait_list[e]]) {
                ncc_list_push(work, wait_list[e]);
            }
        }
    }

    ncc_list_free(work);
    ncc_free(wait_list);
    ncc_free(cursor);
    ncc_free(wait_start);
}

static void
first_deps_of_rule(ncc_grammar_t       *g,
                   ncc_parse_rule_t    *rule,
                   const bool          *first_nullable,
                   ncc_list_t(int32_t) *out)
{
    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        ncc_match_t *m = &rule->contents.data[i];

        switch (m->kind) {
        case NCC_MATCH_TERMINAL:
        case NCC_MATCH_ANY:
        case NCC_MATCH_CLASS:
        case NCC_MATCH_SET:
            return;

        case NCC_MATCH_EMPTY:
            break;

        case NCC_MATCH_GROUP: {
            ncc_rule_group_t *grp = (ncc_rule_group_t *)m->group;
            ncc_nonterm_t    *gnt = ncc_get_nonterm(g, grp->contents_id);

            if (gnt) {
                ncc_list_push(*out, (int32_t)grp->contents_id);
            }

            if (grp->min > 0) {
                return;
            }

            break;
        }

        case NCC_MATCH_NT: {
            ncc_nonterm_t *nt = ncc_get_nonterm(g, m->nt_id);

            if (!nt) {
                return;
            }

            ncc_list_push(*out, (int32_t)m->nt_id);

            if (!first_nullable[m->nt_id]) {
                return;
            }

            break;
        }
        }
    }
}

static bool
refresh_nt_first(ncc_grammar_t *g, ncc_nonterm_t *nt)
{
    size_t old_len = nt->first_set->count;
    bool   old_any = nt->first_has_any;

    for (size_t j = 0; j < nt->rule_ids.len; j++) {
        ncc_parse_rule_t *rule = ncc_get_rule(g, nt->rule_ids.data[j]);

        merge_dict_set_into(nt->first_set, rule->first_set);

        if (rule->first_has_any) {
            nt->first_has_any = true;
        }
    }

    return nt->first_set->count != old_len || nt->first_has_any != old_any;
}

typedef struct {
    ncc_list_t(int32_t) edges;
    int32_t            *start;
} first_deps_t;

static first_deps_t
build_first_deps(ncc_grammar_t *g, bool *first_nullable)
{
    size_t       n_nts = g->nt_list.len;
    first_deps_t d     = {
        .edges = ncc_list_new_cap(int32_t, n_nts * 2),
        .start = ncc_alloc_array(int32_t, n_nts + 1),
    };

    for (size_t i = 0; i < n_nts; i++) {
        ncc_nonterm_t *nt = ncc_get_nonterm(g, (int64_t)i);

        d.start[i] = (int32_t)ncc_list_len(d.edges);

        for (size_t j = 0; j < nt->rule_ids.len; j++) {
            first_deps_of_rule(g,
                               ncc_get_rule(g, nt->rule_ids.data[j]),
                               first_nullable,
                               &d.edges);
        }
    }

    d.start[n_nts] = (int32_t)ncc_list_len(d.edges);

    return d;
}

static void
settle_component(ncc_grammar_t *g, ncc_list_t(int32_t) *members,
                 bool *first_nullable)
{
    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t k = 0; k < ncc_list_len(*members); k++) {
            int32_t        m  = members->data[k];
            ncc_nonterm_t *nt = ncc_get_nonterm(g, m);

            for (size_t j = 0; j < nt->rule_ids.len; j++) {
                ncc_parse_rule_t *r = ncc_get_rule(g, nt->rule_ids.data[j]);

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
first_sets_by_scc(ncc_grammar_t *g, bool *first_nullable)
{
    size_t n_nts = g->nt_list.len;

    if (n_nts == 0) {
        return;
    }

    first_deps_t deps     = build_first_deps(g, first_nullable);
    int32_t     *index    = ncc_alloc_array(int32_t, n_nts);
    int32_t     *low      = ncc_alloc_array(int32_t, n_nts);
    bool        *on_stack = ncc_alloc_array(bool, n_nts);

    for (size_t i = 0; i < n_nts; i++) {
        index[i] = -1;
    }

    ncc_list_t(int32_t) sccstack = ncc_list_new_cap(int32_t, n_nts);
    ncc_list_t(int32_t) frame_v  = ncc_list_new_cap(int32_t, n_nts);
    ncc_list_t(int32_t) frame_e  = ncc_list_new_cap(int32_t, n_nts);
    ncc_list_t(int32_t) members  = ncc_list_new_cap(int32_t, n_nts);

    int32_t next_index = 0;

    for (size_t root = 0; root < n_nts; root++) {
        if (index[root] >= 0) {
            continue;
        }

        ncc_list_push(frame_v, (int32_t)root);
        ncc_list_push(frame_e, deps.start[root]);

        index[root] = next_index;
        low[root]   = next_index;
        next_index++;
        on_stack[root] = true;
        ncc_list_push(sccstack, (int32_t)root);

        while (ncc_list_len(frame_v) > 0) {
            size_t  top = ncc_list_len(frame_v) - 1;
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
                    ncc_list_push(sccstack, w);
                    ncc_list_push(frame_v, w);
                    ncc_list_push(frame_e, deps.start[w]);
                }
                else if (on_stack[w] && low[v] > index[w]) {
                    low[v] = index[w];
                }

                continue;
            }

            ncc_list_pop(int32_t, frame_v);
            ncc_list_pop(int32_t, frame_e);

            if (ncc_list_len(frame_v) > 0) {
                size_t  parent = ncc_list_len(frame_v) - 1;
                int32_t pv     = frame_v.data[parent];

                if (low[pv] > low[v]) {
                    low[pv] = low[v];
                }
            }

            if (low[v] != index[v]) {
                continue;
            }

            ncc_list_clear(members);

            while (true) {
                int32_t m = sccstack.data[ncc_list_len(sccstack) - 1];

                ncc_list_pop(int32_t, sccstack);
                on_stack[m] = false;
                ncc_list_push(members, m);

                if (m == v) {
                    break;
                }
            }

            settle_component(g, &members, first_nullable);
        }
    }

    ncc_list_free(members);
    ncc_list_free(frame_e);
    ncc_list_free(frame_v);
    ncc_list_free(sccstack);
    ncc_free(on_stack);
    ncc_free(low);
    ncc_free(index);
    ncc_free(deps.start);
    ncc_list_free(deps.edges);
}

static void
compute_all_first_sets(ncc_grammar_t *g)
{
    size_t n_nts   = g->nt_list.len;
    size_t n_rules = g->rules.len;

    bool *first_nullable = ncc_alloc_array(bool, n_nts);

    compute_first_nullable(g, first_nullable);

    for (size_t i = 0; i < n_nts; i++) {
        ncc_nonterm_t *nt = ncc_get_nonterm(g, (int64_t)i);

        nt->first_set     = ncc_dict_set_new();
        nt->first_has_any = false;
    }

    for (size_t i = 0; i < n_rules; i++) {
        ncc_parse_rule_t *rule = ncc_get_rule(g, (int32_t)i);

        rule->first_set     = ncc_dict_set_new();
        rule->first_has_any = false;
    }

    first_sets_by_scc(g, first_nullable);

    for (size_t i = 0; i < n_nts; i++) {
        if (first_nullable[i]) {
            ncc_get_nonterm(g, (int64_t)i)->first_has_any = true;
        }
    }

    ncc_free(first_nullable);
}

void
ncc_grammar_finalize(ncc_grammar_t *g)
{
    if (g->finalized) {
        return;
    }

    size_t n = g->nt_list.len;

    for (size_t i = 0; i < n; i++) {
        ncc_list_t(int) stack = ncc_list_new(int);
        is_nullable_nt(g, (int64_t)i, &stack);
        ncc_list_free(stack);
    }

    if (g->error_rules) {
        n = g->rules.len;

        for (size_t i = 0; i < n; i++) {
            create_one_error_rule_set(g, (int32_t)i);
        }
    }

    compute_all_first_sets(g);

    ncc_nonterm_t *start = ncc_get_nonterm(g, g->default_start);

    if (start) {
        start->start_nt = true;
    }

    g->finalized = true;
}
