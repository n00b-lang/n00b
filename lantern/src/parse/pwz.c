
#include "parse/pwz.h"
#include "parse/annotation.h"
#include "parse/tree.h"
#include "private/parse/pwz.h"
#include "private/parse/grammar.h"
#include "private/parse/unicode_class.h"
#include "lib/alloc.h"
#include "lib/array.h"
#include "lib/list.h"
#include "scanner/token_stream.h"
#include "scanner/lex.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline parse_token_info_t *
get_token(parse_pwz_parser_t *p, int32_t pos)
{
    if (pos < 0 || pos >= p->stream_end - p->stream_first) {
        return nullptr;
    }
    return scan_stream_get(p->stream, p->stream_first + pos);
}

static pwz_mem_t *
alloc_mem(parse_pwz_parser_t *p)
{
    pwz_mem_t *m;

    if (p->free_mems) {
        m = p->free_mems;
        p->free_mems = (pwz_mem_t *)m->parents;
    }
    else {
        m = core_arena_alloc(&p->parse_arena, sizeof(pwz_mem_t));
        m->mark = 0;
        core_list_push(p->arena_mems, m);
        p->mems_since_collect++;
    }

    m->parents   = nullptr;
    m->result    = p->exp_bottom;
    m->start_pos = PWZ_POS_BOTTOM;
    m->end_pos   = PWZ_POS_BOTTOM;
    m->refcount  = 1;

    return m;
}

static inline pwz_mem_t *
mem_retain(pwz_mem_t *m)
{
    if (m) {
        m->refcount++;
    }
    return m;
}

static void mem_release(parse_pwz_parser_t *p, pwz_mem_t *m);

static inline void
cxt_release_mem(parse_pwz_parser_t *p, pwz_cxt_t *c)
{
    switch (c->kind) {
    case PWZ_CXT_SEQ: mem_release(p, c->seq.mem); break;
    case PWZ_CXT_ALT: mem_release(p, c->alt.mem); break;
    case PWZ_CXT_TOP: break;
    }
}

static void
mem_release(parse_pwz_parser_t *p, pwz_mem_t *m)
{
    if (!m || --m->refcount > 0) {
        return;
    }
    pwz_cxt_node_t *node = m->parents;
    while (node) {
        pwz_cxt_node_t *next = node->next;
        cxt_release_mem(p, node->cxt);
        core_list_push(p->pending_cxts, node->cxt);
        node->next           = p->pending_cxt_nodes;
        p->pending_cxt_nodes = node;
        node = next;
    }
    m->parents = (pwz_cxt_node_t *)p->free_mems;
    p->free_mems = m;
}

static pwz_cxt_t *
alloc_cxt(parse_pwz_parser_t *p)
{
    pwz_cxt_t *c = p->free_cxts.len
        ? p->free_cxts.data[--p->free_cxts.len]
        : core_arena_alloc(&p->parse_arena, sizeof(pwz_cxt_t));
    *c = (pwz_cxt_t){0};
    return c;
}

static void
recycle_released_cxts(parse_pwz_parser_t *p)
{
    while (p->pending_cxt_nodes) {
        pwz_cxt_node_t *node = p->pending_cxt_nodes;
        p->pending_cxt_nodes = node->next;
        node->next           = p->free_cxt_nodes;
        p->free_cxt_nodes    = node;
    }
    for (size_t i = 0; i < p->pending_cxts.len; i++)
        core_list_push(p->free_cxts, p->pending_cxts.data[i]);
    core_list_clear(p->pending_cxts);
}

static void
drop_released_cxts(parse_pwz_parser_t *p)
{
    p->free_cxt_nodes    = nullptr;
    p->pending_cxt_nodes = nullptr;
    core_list_clear(p->free_cxts);
    core_list_clear(p->pending_cxts);
    core_list_clear(p->arena_mems);
    p->mems_since_collect = 0;
}

static pwz_mem_t *
cxt_mem(pwz_cxt_t *c)
{
    switch (c->kind) {
    case PWZ_CXT_SEQ: return c->seq.mem;
    case PWZ_CXT_ALT: return c->alt.mem;
    case PWZ_CXT_TOP: return nullptr;
    }
    return nullptr;
}

static size_t
mark_reachable_mems(parse_pwz_parser_t *p, uint32_t epoch)
{
    size_t live = 0;
    core_list_clear(p->mark_stack);
    for (size_t i = 0; i < p->worklist.len; i++)
        core_list_push(p->mark_stack, p->worklist.data[i].mem);
    while (p->mark_stack.len) {
        pwz_mem_t *m = p->mark_stack.data[--p->mark_stack.len];
        if (!m || m->mark == epoch) continue;
        m->mark = epoch;
        live++;
        for (pwz_cxt_node_t *node = m->parents; node; node = node->next)
            core_list_push(p->mark_stack, cxt_mem(node->cxt));
    }
    return live;
}

static void
free_unreachable_mem(parse_pwz_parser_t *p, pwz_mem_t *m, uint32_t epoch)
{
    pwz_cxt_node_t *node = m->parents;
    while (node) {
        pwz_cxt_node_t *next = node->next;
        pwz_mem_t      *held = cxt_mem(node->cxt);
        if (held && held->mark == epoch) mem_release(p, held);
        core_list_push(p->free_cxts, node->cxt);
        node->next        = p->free_cxt_nodes;
        p->free_cxt_nodes = node;
        node              = next;
    }
    m->refcount  = 0;
    m->parents   = (pwz_cxt_node_t *)p->free_mems;
    p->free_mems = m;
}

static void
collect_unreachable_mems(parse_pwz_parser_t *p)
{
    uint32_t epoch = ++p->mark_epoch;
    size_t   live  = mark_reachable_mems(p, epoch);
    for (size_t i = 0; i < p->all_exps.len; i++) {
        pwz_exp_t *exp = p->all_exps.data[i];
        if (exp->mem && exp->mem->mark != epoch) exp->mem = nullptr;
    }
    for (size_t i = 0; i < p->arena_mems.len; i++) {
        pwz_mem_t *m = p->arena_mems.data[i];
        if (m->refcount && m->mark != epoch) free_unreachable_mem(p, m, epoch);
    }
    p->mems_since_collect = 0;
    p->collect_threshold  = live * 2 > 65536 ? live * 2 : 65536;
}

static pwz_cxt_node_t *
alloc_cxt_node(parse_pwz_parser_t *p, pwz_cxt_t *cxt, pwz_cxt_node_t *next)
{
    pwz_cxt_node_t *n = p->free_cxt_nodes;
    if (n) p->free_cxt_nodes = n->next;
    else n = core_arena_alloc(&p->parse_arena, sizeof(pwz_cxt_node_t));

    n->cxt  = cxt;
    n->next = next;

    return n;
}

static pwz_exp_t *
alloc_result_exp(parse_pwz_parser_t *p)
{
    return core_arena_alloc(&p->parse_arena, sizeof(pwz_exp_t));
}

static void
register_exp(parse_pwz_parser_t *p, pwz_exp_t *e)
{
    core_list_push(p->all_exps, e);
}

static pwz_exp_t *
make_tok_exp(parse_pwz_parser_t *p, parse_grammar_t *g, int64_t tid)
{
    pwz_exp_t *e = core_alloc(pwz_exp_t);
    int32_t contextual = parse_contextual_source_tid(g, tid);

    e->kind      = PWZ_TOK;
    e->tid       = tid;
    e->match_tid = contextual ? contextual : (int32_t)tid;
    if (contextual) {
        e->contextual_spelling = parse_terminal_name(g, tid);
    }

    register_exp(p, e);
    return e;
}

static pwz_exp_t *
make_class_exp(parse_pwz_parser_t *p, parse_char_class_t cc)
{
    pwz_exp_t *e = core_alloc(pwz_exp_t);

    e->kind   = PWZ_CLASS;
    e->cc = cc;

    register_exp(p, e);
    return e;
}

static pwz_exp_t *
make_any_exp(parse_pwz_parser_t *p)
{
    pwz_exp_t *e = core_alloc(pwz_exp_t);

    e->kind = PWZ_ANY;

    register_exp(p, e);
    return e;
}

static pwz_exp_t *
make_set_exp(parse_pwz_parser_t *p, const parse_match_set_t *set)
{
    pwz_exp_t *e = core_alloc(pwz_exp_t);

    e->kind = PWZ_SET;
    e->set  = set;

    register_exp(p, e);
    return e;
}

static pwz_exp_t *
make_seq_exp(parse_pwz_parser_t *p,
             core_string_t       name,
             int64_t            nt_id,
             int32_t            rule_ix,
             pwz_exp_ptr_t     *children,
             int32_t            nchildren)
{
    pwz_exp_t *e = core_alloc(pwz_exp_t);

    e->kind          = PWZ_SEQ;
    e->seq.name      = name;
    e->nt_id     = nt_id;
    e->rule_ix   = rule_ix;
    e->seq.children  = children;
    e->nchildren = nchildren;

    register_exp(p, e);
    return e;
}

static pwz_exp_t *
make_alt_exp(parse_pwz_parser_t *p, int64_t nt_id)
{
    pwz_exp_t *e = core_alloc(pwz_exp_t);

    e->kind      = PWZ_ALT;
    e->nt_id = nt_id;
    e->alt.alts  = core_list_new(pwz_exp_ptr_t);

    register_exp(p, e);
    return e;
}

static void
alt_add(pwz_exp_t *alt, pwz_exp_t *child)
{
    core_list_push(alt->alt.alts, child);
}

static void
build_seq_children(parse_pwz_parser_t *p,
                   parse_rule_t *rule,
                   pwz_exp_ptr_t    **out_children,
                   int32_t           *out_count)
{
    size_t n = core_list_len(rule->contents);

    int32_t count = 0;

    for (size_t i = 0; i < n; i++) {
        parse_match_t *item = &rule->contents.data[i];

        if (item->kind != PARSE_MATCH_EMPTY) {
            count++;
        }
    }

    if (count == 0) {
        *out_children = nullptr;
        *out_count    = 0;
        return;
    }

    pwz_exp_ptr_t *children = core_alloc_array(pwz_exp_ptr_t, count);
    int32_t        ix       = 0;

    for (size_t i = 0; i < n; i++) {
        parse_match_t *item = &rule->contents.data[i];

        switch (item->kind) {
        case PARSE_MATCH_NT:
            children[ix++] = p->nt_exps[item->nt_id];
            break;

        case PARSE_MATCH_TERMINAL:
            children[ix++] = make_tok_exp(p, p->grammar, item->terminal_id);
            break;

        case PARSE_MATCH_CLASS:
            children[ix++] = make_class_exp(p, item->char_class);
            break;

        case PARSE_MATCH_ANY:
            children[ix++] = make_any_exp(p);
            break;

        case PARSE_MATCH_GROUP: {
            parse_rule_group_t *grp = (parse_rule_group_t *)item->group;
            children[ix++]         = p->nt_exps[grp->contents_id];
            break;
        }

        case PARSE_MATCH_SET:
            children[ix++] = make_set_exp(
                p, (const parse_match_set_t *)item->set_items);
            break;

        case PARSE_MATCH_EMPTY:
            break;
        }
    }

    *out_children = children;
    *out_count    = ix;
}

static void
expand_group_nt(parse_pwz_parser_t *p, parse_grammar_t *g, int64_t nt_id)
{
    parse_nonterm_t *nt  = parse_get_nonterm(g, nt_id);
    pwz_exp_t      *alt = p->nt_exps[nt_id];

    parse_rule_group_t *grp = nullptr;

    for (size_t i = 0; i < core_list_len(g->rules); i++) {
        parse_rule_t *rule = &g->rules.data[i];

        for (size_t j = 0; j < core_list_len(rule->contents); j++) {
            parse_match_t *item = &rule->contents.data[j];

            if (item->kind == PARSE_MATCH_GROUP) {
                parse_rule_group_t *rg = (parse_rule_group_t *)item->group;

                if (rg->contents_id == nt_id) {
                    grp = rg;
                    break;
                }
            }
        }

        if (grp) {
            break;
        }
    }

    if (!grp) {
        for (size_t i = 0; i < core_list_len(nt->rule_ids); i++) {
            int32_t            rule_ix = nt->rule_ids.data[i];
            parse_rule_t *rule    = parse_get_rule(g, rule_ix);

            if (rule->penalty_rule) {
                continue;
            }

            pwz_exp_ptr_t *children;
            int32_t        nchildren;

            build_seq_children(p, rule, &children, &nchildren);

            pwz_exp_t *seq = make_seq_exp(p, nt->name, nt_id, (int32_t)i,
                                          children, nchildren);
            alt_add(alt, seq);
        }

        return;
    }

    pwz_exp_t *body_alt = make_alt_exp(p, nt_id);

    for (size_t i = 0; i < core_list_len(nt->rule_ids); i++) {
        int32_t            rule_ix = nt->rule_ids.data[i];
        parse_rule_t *rule    = parse_get_rule(g, rule_ix);

        if (rule->penalty_rule) {
            continue;
        }

        pwz_exp_ptr_t *children;
        int32_t        nchildren;

        build_seq_children(p, rule, &children, &nchildren);

        pwz_exp_t *seq = make_seq_exp(p, nt->name, nt_id, (int32_t)i,
                                      children, nchildren);
        alt_add(body_alt, seq);
    }

    pwz_exp_t *empty_seq = make_seq_exp(p, nt->name, nt_id, -1, nullptr, 0);

    size_t body_nalts = core_list_len(body_alt->alt.alts);

    if (grp->min == 0 && grp->max == 1) {
        for (size_t i = 0; i < body_nalts; i++) {
            alt_add(alt, body_alt->alt.alts.data[i]);
        }

        alt_add(alt, empty_seq);
    }
    else if (grp->min == 0 && grp->max == 0) {
        for (size_t i = 0; i < body_nalts; i++) {
            pwz_exp_t *body_seq = body_alt->alt.alts.data[i];
            int32_t    nc       = body_seq->nchildren;
            int32_t    new_nc   = nc + 1;

            pwz_exp_ptr_t *new_children = core_alloc_array(pwz_exp_ptr_t, new_nc);
            new_children[0] = alt;
            memcpy(new_children + 1, body_seq->seq.children,
                   (size_t)nc * sizeof(pwz_exp_ptr_t));

            pwz_exp_t *rep_seq = make_seq_exp(p, nt->name, nt_id,
                                              body_seq->rule_ix,
                                              new_children, new_nc);
            alt_add(alt, rep_seq);
        }

        alt_add(alt, empty_seq);
    }
    else if (grp->min == 1 && grp->max == 0) {
        for (size_t i = 0; i < body_nalts; i++) {
            pwz_exp_t *body_seq = body_alt->alt.alts.data[i];
            int32_t    nc       = body_seq->nchildren;
            int32_t    new_nc   = nc + 1;

            pwz_exp_ptr_t *new_children = core_alloc_array(pwz_exp_ptr_t, new_nc);
            new_children[0] = alt;
            memcpy(new_children + 1, body_seq->seq.children,
                   (size_t)nc * sizeof(pwz_exp_ptr_t));

            pwz_exp_t *rep_seq = make_seq_exp(p, nt->name, nt_id,
                                              body_seq->rule_ix,
                                              new_children, new_nc);
            alt_add(alt, rep_seq);
        }

        for (size_t i = 0; i < body_nalts; i++) {
            alt_add(alt, body_alt->alt.alts.data[i]);
        }
    }
    else {
        for (size_t i = 0; i < body_nalts; i++) {
            alt_add(alt, body_alt->alt.alts.data[i]);
        }
    }
}

static void
expand_list_nt(parse_pwz_parser_t *p, parse_grammar_t *g, int32_t nt_id)
{
    parse_nonterm_t *nt    = parse_get_nonterm(g, nt_id);
    pwz_exp_t       *steps = make_alt_exp(p, -1);

    for (size_t j = 0; j < core_list_len(nt->rule_ids); j++) {
        int32_t       rule_ix = nt->rule_ids.data[j];
        parse_rule_t *rule    = parse_get_rule(g, rule_ix);
        bool          step    = parse_rule_is_list_step(g, rule_ix);

        pwz_exp_ptr_t *children;
        int32_t        nchildren;

        build_seq_children(p, rule, &children, &nchildren);

        int32_t        kept  = step ? nchildren - 1 : nchildren;
        pwz_exp_ptr_t *after = core_alloc_array(pwz_exp_ptr_t, kept + 1);

        after[0] = steps;
        memcpy(after + 1, children, (size_t)kept * sizeof(pwz_exp_ptr_t));
        core_free(children);

        pwz_exp_t *seq = make_seq_exp(p, nt->name, nt_id, (int32_t)j, after,
                                      kept + 1);

        seq->list_role = step ? PWZ_LIST_STEP : PWZ_LIST_END;
        alt_add(step ? steps : p->nt_exps[nt_id], seq);
    }

    pwz_exp_t *none = make_seq_exp(p, nt->name, -1, -1, nullptr, 0);

    none->list_role = PWZ_LIST_STEP;
    alt_add(steps, none);
}

static void
build_exp_graph(parse_pwz_parser_t *p, parse_grammar_t *g,
                parse_nt_id_t start)
{
    int32_t num_nts = (int32_t)core_list_len(g->nt_list);

    p->nt_exps = core_alloc_array(pwz_exp_ptr_t, num_nts);

    for (int32_t i = 0; i < num_nts; i++) {
        p->nt_exps[i] = make_alt_exp(p, i);
    }

    for (int32_t i = 0; i < num_nts; i++) {
        parse_nonterm_t *nt = parse_get_nonterm(g, i);

        if (nt->group_nt) {
            expand_group_nt(p, g, i);
            continue;
        }

        if (parse_nonterm_is_quadratic_list(g, nt)) {
            expand_list_nt(p, g, i);
            continue;
        }

        pwz_exp_t *alt_node = p->nt_exps[i];

        for (size_t j = 0; j < core_list_len(nt->rule_ids); j++) {
            int32_t            rule_ix = nt->rule_ids.data[j];
            parse_rule_t *rule    = parse_get_rule(g, rule_ix);

            if (rule->penalty_rule) {
                continue;
            }

            pwz_exp_ptr_t *children;
            int32_t        nchildren;

            build_seq_children(p, rule, &children, &nchildren);

            pwz_exp_t *seq = make_seq_exp(p, nt->name, (int64_t)i,
                                          (int32_t)j, children, nchildren);
            alt_add(alt_node, seq);
        }
    }

    p->start_exp = p->nt_exps[start];
}

static void
reset_memos(parse_pwz_parser_t *p)
{
    size_t num = core_list_len(p->all_exps);

    for (size_t i = 0; i < num; i++) {
        mem_release(p, p->all_exps.data[i]->mem);
        p->all_exps.data[i]->mem = nullptr;
    }
}

#define TERM_TO_PTR(id) ((void *)(uintptr_t)((uint64_t)(id) + 0x100))

static inline bool
rule_first_matches(parse_rule_t *rule, int64_t token_id)
{
    if (rule->nullable || rule->first_has_any) {
        return true;
    }

    if (!rule->first_set || rule->first_set->count == 0) {
        return true;
    }

    return core_dict_contains(rule->first_set, TERM_TO_PTR(token_id));
}

static void
push_step(parse_pwz_parser_t *p, pwz_step_t s)
{
    core_list_push(p->steps, s);
}

static void
reverse_steps(parse_pwz_parser_t *p, size_t mark)
{
    size_t lo = mark;
    size_t hi = p->steps.len;

    while (hi > lo + 1) {
        hi--;

        pwz_step_t tmp    = p->steps.data[lo];
        p->steps.data[lo] = p->steps.data[hi];
        p->steps.data[hi] = tmp;
        lo++;
    }
}

static void
step_dd(parse_pwz_parser_t *p, int32_t pos, parse_token_info_t *tok,
        pwz_cxt_t *cxt, pwz_exp_t *exp)
{
    push_step(p, (pwz_step_t){.kind = PWZ_STEP_DD,
                              .pos  = pos,
                              .tok  = tok,
                              .cxt  = cxt,
                              .exp  = exp});
}

static void
step_ddp(parse_pwz_parser_t *p, int32_t pos, parse_token_info_t *tok,
         pwz_mem_t *mem, pwz_exp_t *exp)
{
    push_step(p, (pwz_step_t){.kind = PWZ_STEP_DDP,
                              .pos  = pos,
                              .tok  = tok,
                              .mem  = mem,
                              .exp  = exp});
}

static void
step_du(parse_pwz_parser_t *p, int32_t pos, pwz_exp_t *result, pwz_mem_t *mem)
{
    push_step(p, (pwz_step_t){.kind   = PWZ_STEP_DU,
                              .pos    = pos,
                              .result = result,
                              .mem    = mem});
}

static void
step_dup(parse_pwz_parser_t *p, int32_t pos, pwz_exp_t *result, pwz_cxt_t *cxt)
{
    push_step(p, (pwz_step_t){.kind   = PWZ_STEP_DUP,
                              .pos    = pos,
                              .result = result,
                              .cxt    = cxt});
}

static void run_steps(parse_pwz_parser_t *p, size_t base);

static void
d_d(parse_pwz_parser_t *p, int32_t pos, parse_token_info_t *tok,
    pwz_cxt_t *cxt, pwz_exp_t *exp)
{
    size_t base = p->steps.len;

    step_dd(p, pos, tok, cxt, exp);
    run_steps(p, base);
}

static void
d_u(parse_pwz_parser_t *p, int32_t pos, pwz_exp_t *result, pwz_mem_t *mem)
{
    size_t base = p->steps.len;

    step_du(p, pos, result, mem);
    run_steps(p, base);
}

static bool
token_matches(parse_token_info_t *tok, pwz_exp_t *exp)
{
    if (!tok) {
        return false;
    }

    switch (exp->kind) {
    case PWZ_TOK: {
        if (parse_token_tid(tok) != exp->match_tid) {
            return false;
        }
        if (!exp->contextual_spelling.data) {
            return true;
        }
        core_option_t(core_string_t) value = parse_token_value(tok);
        return core_option_is_set(value)
            && core_string_eq(core_option_get(value),
                              exp->contextual_spelling);
    }

    case PWZ_CLASS:
        return parse_codepoint_matches_class(parse_token_tid(tok), exp->cc);

    case PWZ_ANY:
        return true;

    case PWZ_SET:
        return parse_match_set_holds(exp->set, parse_token_tid(tok));

    default:
        return false;
    }
}

static void
run_dd(parse_pwz_parser_t *p, int32_t pos, parse_token_info_t *tok,
       pwz_cxt_t *cxt, pwz_exp_t *exp)
{
    if (exp->mem && exp->mem->start_pos == pos) {
        exp->mem->parents = alloc_cxt_node(p, cxt, exp->mem->parents);

        if (pwz_mem_end_pos(exp->mem) != PWZ_POS_BOTTOM) {
            step_dup(p, pwz_mem_end_pos(exp->mem), exp->mem->result, cxt);
        }

        return;
    }

    mem_release(p, exp->mem);

    pwz_mem_t *mem = alloc_mem(p);

    mem->start_pos = pos;
    mem->parents   = alloc_cxt_node(p, cxt, nullptr);
    exp->mem       = mem;

    step_ddp(p, pos, tok, mem, exp);
}

static void
record_expected(parse_pwz_parser_t *p, pwz_mem_t *mem, pwz_exp_t *exp)
{
    parse_pwz_expected_t entry = {
        .tid = exp->kind == PWZ_TOK ? exp->match_tid : -1,
        .spelling = exp->contextual_spelling,
    };
    enum { TRACE_LIMIT = 512 };
    struct {
        pwz_mem_t      *mem;
        pwz_cxt_node_t *next;
        pwz_cxt_t      *via;
    } trail[TRACE_LIMIT];
    size_t   depth = 0;
    bool     reached = false;
    uint32_t epoch = ++p->mark_epoch;

    mem->mark = epoch;
    trail[depth++] = (typeof(trail[0])){mem, mem->parents, nullptr};
    while (depth && !reached) {
        pwz_cxt_node_t *node = trail[depth - 1].next;
        if (!node) {
            depth--;
            continue;
        }
        trail[depth - 1].next = node->next;
        pwz_cxt_t *cxt = node->cxt;
        if (!cxt) {
            continue;
        }
        if (cxt->kind == PWZ_CXT_TOP) {
            reached = true;
            break;
        }
        pwz_mem_t *up = cxt->kind == PWZ_CXT_ALT ? cxt->alt.mem : cxt->seq.mem;
        if (!up || up->mark == epoch || depth == TRACE_LIMIT) {
            continue;
        }
        up->mark = epoch;
        trail[depth++] = (typeof(trail[0])){up, up->parents, cxt};
    }

    for (size_t i = 1; reached && i < depth; i++) {
        pwz_cxt_t *cxt = trail[i].via;
        if (cxt->kind != PWZ_CXT_SEQ || cxt->nt_id < 0
            || entry.frame_count == PARSE_PWZ_EXPECTED_DEPTH) {
            continue;
        }
        parse_nonterm_t *nt = parse_get_nonterm(p->grammar, cxt->nt_id);
        if (nt && nt->name.data) {
            entry.frames[entry.frame_count++] = (parse_pwz_rule_frame_t){
                .rule  = nt->name,
                .start = cxt->seq.mem->start_pos + p->stream_first,
            };
        }
    }
    core_list_push(p->expected, entry);
}

static void
run_ddp(parse_pwz_parser_t *p, int32_t pos, parse_token_info_t *tok,
        pwz_mem_t *mem, pwz_exp_t *exp)
{
    switch (exp->kind) {
    case PWZ_TOK:
    case PWZ_CLASS:
    case PWZ_ANY:
    case PWZ_SET:
        if (pos == p->expected_pos && !token_matches(tok, exp)) {
            record_expected(p, mem, exp);
        }
        if (token_matches(tok, exp)) {
            pwz_exp_t *result = alloc_result_exp(p);

            result->kind    = exp->kind;
            result->tid     = parse_token_tid(tok);
            result->match_tid = parse_token_tid(tok);

            core_list_push(p->worklist_swap,
                           ((pwz_zipper_t){.result = result,
                                           .mem = mem_retain(mem)}));
        }

        break;

    case PWZ_SEQ:
        if (exp->nchildren == 0) {
            pwz_exp_t *result = alloc_result_exp(p);

            result->kind          = PWZ_SEQ;
            result->list_role     = exp->list_role;
            result->seq.name      = exp->seq.name;
            result->nt_id     = exp->nt_id;
            result->rule_ix   = exp->rule_ix;
            result->seq.children  = nullptr;
            result->nchildren = 0;

            step_du(p, pos, result, mem);
        }
        else {
            pwz_cxt_t *seq_cxt = alloc_cxt(p);

            seq_cxt->kind        = PWZ_CXT_SEQ;
            seq_cxt->list_role   = exp->list_role;
            seq_cxt->seq.mem     = mem_retain(mem);
            seq_cxt->nt_id   = exp->nt_id;
            seq_cxt->rule_ix = exp->rule_ix;
            seq_cxt->seq.left    = nullptr;
            seq_cxt->nleft   = 0;
            seq_cxt->seq.right   = exp->seq.children + 1;
            seq_cxt->seq.nright  = exp->nchildren - 1;

            step_dd(p, pos, tok, seq_cxt, exp->seq.children[0]);
        }

        break;

    case PWZ_ALT: {
        bool can_filter_alts = false;

        if (tok && exp->nt_id >= 0 && pos != p->expected_pos) {
            parse_nonterm_t *nt = parse_get_nonterm(p->grammar, exp->nt_id);

            if (nt && !nt->group_nt) {
                can_filter_alts = true;
            }
        }

        size_t nalts = core_list_len(exp->alt.alts);
        size_t mark  = p->steps.len;

        for (size_t i = 0; i < nalts; i++) {
            pwz_exp_t *alt_child = exp->alt.alts.data[i];

            if (can_filter_alts && alt_child->kind == PWZ_SEQ
                && alt_child->list_role == PWZ_LIST_NONE
                && alt_child->nt_id >= 0 && alt_child->rule_ix >= 0) {
                parse_nonterm_t *nt = parse_get_nonterm(p->grammar,
                                                      alt_child->nt_id);

                if (nt && alt_child->rule_ix < (int32_t)core_list_len(nt->rule_ids)) {
                    int32_t            rix  = nt->rule_ids.data[alt_child->rule_ix];
                    parse_rule_t *rule = parse_get_rule(p->grammar, rix);

                    if (rule && !rule_first_matches(rule,
                                                    parse_token_tid(tok))) {
                        continue;
                    }
                }
            }

            pwz_cxt_t *alt_cxt = alloc_cxt(p);

            alt_cxt->kind    = PWZ_CXT_ALT;
            alt_cxt->alt.mem = mem_retain(mem);

            step_dd(p, pos, tok, alt_cxt, alt_child);
        }

        reverse_steps(p, mark);

        break;
    }
    }
}

static void
run_du(parse_pwz_parser_t *p, int32_t pos, pwz_exp_t *result, pwz_mem_t *mem)
{
    int32_t ep = pwz_mem_end_pos(mem);

    if (ep != PWZ_POS_BOTTOM) {
        if (pos == ep) {
            pwz_exp_t *existing = mem->result;

            if (existing->kind == PWZ_ALT && existing->nt_id == -1) {
                core_list_push(existing->alt.alts, result);
            }
            else {
                pwz_exp_t *copy = alloc_result_exp(p);

                *copy = *existing;

                existing->kind      = PWZ_ALT;
                existing->nt_id = -1;
                existing->alt.alts  = core_list_new(pwz_exp_ptr_t);
                core_list_push(existing->alt.alts, copy);
                core_list_push(existing->alt.alts, result);
            }

            return;
        }

        if (pwz_mem_in_progress(mem)) {
            return;
        }
    }

    mem->end_pos = pos | PWZ_MEM_IN_PROGRESS_BIT;
    mem->result  = result;

    push_step(p, (pwz_step_t){.kind = PWZ_STEP_DU_DONE, .mem = mem});

    size_t mark = p->steps.len;

    for (pwz_cxt_node_t *node = mem->parents; node; node = node->next) {
        step_dup(p, pos, result, node->cxt);
    }

    reverse_steps(p, mark);
}

static void
run_dup(parse_pwz_parser_t *p, int32_t pos, pwz_exp_t *result, pwz_cxt_t *cxt)
{
    switch (cxt->kind) {
    case PWZ_CXT_TOP:
        core_list_push(p->tops, result);
        break;

    case PWZ_CXT_SEQ: {
        if (cxt->seq.nright == 0) {
            int32_t total = cxt->nleft + 1;

            pwz_exp_ptr_t *children = core_arena_alloc(
                &p->parse_arena, (size_t)total * sizeof(pwz_exp_ptr_t));

            for (int32_t i = 0; i < cxt->nleft; i++) {
                children[i] = cxt->seq.left[i];
            }

            children[cxt->nleft] = result;

            pwz_exp_t *seq_result = alloc_result_exp(p);

            seq_result->kind          = PWZ_SEQ;
            seq_result->list_role     = cxt->list_role;
            seq_result->seq.name      = cxt->nt_id >= 0
                ? parse_get_nonterm(p->grammar, cxt->nt_id)->name
                : (core_string_t){0};
            seq_result->nt_id     = cxt->nt_id;
            seq_result->rule_ix   = cxt->rule_ix;
            seq_result->seq.children  = children;
            seq_result->nchildren = total;

            step_du(p, pos, seq_result, cxt->seq.mem);
        }
        else {
            int32_t new_nleft = cxt->nleft + 1;

            pwz_exp_ptr_t *new_left = core_arena_alloc(
                &p->parse_arena, (size_t)new_nleft * sizeof(pwz_exp_ptr_t));

            for (int32_t i = 0; i < cxt->nleft; i++) {
                new_left[i] = cxt->seq.left[i];
            }

            new_left[cxt->nleft] = result;

            pwz_cxt_t *new_seq_cxt = alloc_cxt(p);

            new_seq_cxt->kind        = PWZ_CXT_SEQ;
            new_seq_cxt->list_role   = cxt->list_role;
            new_seq_cxt->seq.mem     = mem_retain(cxt->seq.mem);
            new_seq_cxt->nt_id   = cxt->nt_id;
            new_seq_cxt->rule_ix = cxt->rule_ix;
            new_seq_cxt->seq.left    = new_left;
            new_seq_cxt->nleft   = new_nleft;
            new_seq_cxt->seq.right   = cxt->seq.right + 1;
            new_seq_cxt->seq.nright  = cxt->seq.nright - 1;

            step_dd(p, pos, get_token(p, pos), new_seq_cxt,
                    cxt->seq.right[0]);
        }

        break;
    }

    case PWZ_CXT_ALT:
        step_du(p, pos, result, cxt->alt.mem);
        break;
    }
}

static void
run_steps(parse_pwz_parser_t *p, size_t base)
{
    while (p->steps.len > base) {
        pwz_step_t s = p->steps.data[--p->steps.len];

        switch (s.kind) {
        case PWZ_STEP_DD:
            run_dd(p, s.pos, s.tok, s.cxt, s.exp);
            break;

        case PWZ_STEP_DDP:
            run_ddp(p, s.pos, s.tok, s.mem, s.exp);
            break;

        case PWZ_STEP_DU:
            run_du(p, s.pos, s.result, s.mem);
            break;

        case PWZ_STEP_DUP:
            run_dup(p, s.pos, s.result, s.cxt);
            break;

        case PWZ_STEP_DU_DONE:
            pwz_mem_set_in_progress(s.mem, false);
            break;
        }
    }
}

static void
init_parse(parse_pwz_parser_t *p)
{
    reset_memos(p);
    core_list_clear(p->worklist);
    core_list_clear(p->worklist_swap);
    core_list_clear(p->tops);
    core_list_clear(p->steps);
    p->longest_top  = nullptr;
    p->longest_end  = 0;
    p->result_tree  = nullptr;
    p->result_trees = (parse_tree_array_t){0};

    pwz_mem_t *mem_top = alloc_mem(p);

    mem_top->start_pos = 0;

    pwz_cxt_t *top_cxt = alloc_cxt(p);

    top_cxt->kind    = PWZ_CXT_TOP;
    mem_top->parents = alloc_cxt_node(p, top_cxt, nullptr);

    pwz_cxt_t *alt_cxt = alloc_cxt(p);

    alt_cxt->kind    = PWZ_CXT_ALT;
    alt_cxt->alt.mem = mem_retain(mem_top);

    parse_token_info_t *tok = get_token(p, 0);

    d_d(p, 0, tok, alt_cxt, p->start_exp);
}

static bool
tid_in(core_list_t(int32_t) tids, parse_token_info_t *tok)
{
    if (!tok) {
        return false;
    }

    int32_t tid = parse_token_tid(tok);

    for (size_t i = 0; i < core_list_len(tids); i++) {
        if (tids.data[i] == tid) {
            return true;
        }
    }

    return false;
}

static bool
skipped_at(parse_pwz_parser_t *p, int32_t pos)
{
    size_t lo = 0;
    size_t hi = core_list_len(p->skipped);

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->skipped.data[mid] < pos) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }

    return lo < core_list_len(p->skipped) && p->skipped.data[lo] == pos;
}

static int32_t
live_pos(parse_pwz_parser_t *p, int32_t pos)
{
    while (skipped_at(p, pos)) {
        pos++;
    }

    return pos;
}

static void
release_worklist(parse_pwz_parser_t *p, size_t wl_len)
{
    for (size_t i = 0; i < wl_len; i++) {
        mem_release(p, p->worklist.data[i].mem);
    }
}

static void
retain_worklist(parse_pwz_parser_t *p, int32_t pos, size_t wl_len)
{
    core_list_push(p->skipped, pos);
    for (size_t i = 0; i < wl_len; i++) {
        pwz_zipper_t *z = &p->worklist.data[i];
        core_list_push(p->worklist_swap,
                       ((pwz_zipper_t){
                           .result = z->result,
                           .mem = mem_retain(z->mem),
                       }));
    }
    release_worklist(p, wl_len);
}

static void
continue_line(parse_pwz_parser_t *p, int32_t pos, size_t wl_len)
{
    parse_token_info_t *next = get_token(p, pos + 1);

    if (next && parse_token_tid(next) == p->indent_tid) {
        p->continuation_indent_pos = pos + 1;
    }
    p->end_run = PWZ_RUN_CONTINUED;
    retain_worklist(p, pos, wl_len);
}

static bool
trivia_has_comment(parse_trivia_t *trivia)
{
    for (parse_trivia_t *item = trivia; item; item = item->next) {
        if (item->kind == PARSE_TRIVIA_COMMENT) {
            return true;
        }
    }

    return false;
}

static bool
newline_has_comment(parse_pwz_parser_t *p, int32_t pos)
{
    parse_token_info_t *before = get_token(p, pos - 1);
    parse_token_info_t *newline = get_token(p, pos);
    parse_token_info_t *after = get_token(p, pos + 1);

    return (before && trivia_has_comment(parse_token_trailing_trivia(before)))
        || trivia_has_comment(parse_token_leading_trivia(newline))
        || trivia_has_comment(parse_token_trailing_trivia(newline))
        || (after && trivia_has_comment(parse_token_leading_trivia(after)));
}

static void
drop_continuation(parse_pwz_parser_t *p)
{
    for (size_t i = 0; i < core_list_len(p->held); i++) {
        mem_release(p, p->held.data[i].mem);
    }
    core_list_clear(p->held);
    p->held_pos = -1;
}

static void
hold_continuation(parse_pwz_parser_t *p, int32_t pos, size_t wl_len)
{
    drop_continuation(p);
    for (size_t i = 0; i < wl_len; i++) {
        pwz_zipper_t *z = &p->worklist.data[i];
        core_list_push(p->held, ((pwz_zipper_t){
                                    .result = z->result,
                                    .mem = mem_retain(z->mem),
                                }));
    }
    p->held_pos = pos;
}

static bool
apply_layout(parse_pwz_parser_t *p, int32_t pos, parse_token_info_t *tok,
             size_t wl_len)
{
    int32_t tid = parse_token_tid(tok);

    if (tid == p->indent_tid && pos == p->continuation_indent_pos) {
        p->continuation_indents++;
        retain_worklist(p, pos, wl_len);
        return true;
    }

    if (tid == p->dedent_tid && p->continuation_indents > 0) {
        p->continuation_indents--;
        retain_worklist(p, pos, wl_len);
        return true;
    }

    if (!tid_in(p->statement_ends, tok)) {
        return false;
    }

    bool newline = tid == p->newline_tid;
    bool commented = newline && newline_has_comment(p, pos);

    if (newline && !commented && p->end_run == PWZ_RUN_NONE
        && tid_in(p->continue_after, get_token(p, pos - 1))) {
        continue_line(p, pos, wl_len);
        return true;
    }

    for (size_t i = 0; i < wl_len; i++) {
        pwz_zipper_t *z = &p->worklist.data[i];
        d_u(p, pos, z->result, z->mem);
    }

    if (p->worklist_swap.len || p->tops.len) {
        if (newline && !commented && p->end_run == PWZ_RUN_NONE) {
            hold_continuation(p, pos, wl_len);
        }
        p->end_run = PWZ_RUN_ENDED;
        release_worklist(p, wl_len);
        if (core_list_len(p->tops) > 0) {
            p->longest_top = p->tops.data[0];
            p->longest_end = pos;
        }
        return true;
    }

    if (p->end_run == PWZ_RUN_ENDED) {
        retain_worklist(p, pos, wl_len);
        return true;
    }

    if (newline && !commented && p->end_run == PWZ_RUN_NONE) {
        continue_line(p, pos, wl_len);
        return true;
    }

    p->error_unterminated = p->end_run == PWZ_RUN_CONTINUED;
    p->error_commented = commented && p->end_run == PWZ_RUN_NONE;
    release_worklist(p, wl_len);
    return true;
}

static void
step_input(parse_pwz_parser_t *p, int32_t complete_pos,
           parse_token_info_t *input, size_t wl_len)
{
    if (input && core_list_len(p->statement_ends)
        && apply_layout(p, complete_pos, input, wl_len)) {
        return;
    }
    p->end_run = PWZ_RUN_NONE;

    for (size_t i = 0; i < wl_len; i++) {
        pwz_zipper_t *z = &p->worklist.data[i];
        size_t before = p->worklist_swap.len;
        size_t before_tops = p->tops.len;
        d_u(p, complete_pos, z->result, z->mem);
        if (input && parse_token_ignore_if_no_match(input)
            && p->worklist_swap.len == before
            && p->tops.len == before_tops) {
            core_list_push(p->worklist_swap,
                           ((pwz_zipper_t){
                               .result = z->result,
                               .mem = mem_retain(z->mem),
                           }));
        }
    }

    release_worklist(p, wl_len);

    if (core_list_len(p->tops) > 0) {
        p->longest_top = p->tops.data[0];
        p->longest_end = complete_pos;
    }
}

static void
resume_continuation(parse_pwz_parser_t *p, int32_t complete_pos,
                    parse_token_info_t *input)
{
    core_list_push(p->skipped, p->held_pos);
    core_list_clear(p->worklist);
    for (size_t i = 0; i < core_list_len(p->held); i++) {
        core_list_push(p->worklist, p->held.data[i]);
    }
    core_list_clear(p->held);
    p->held_pos = -1;
    p->end_run = PWZ_RUN_CONTINUED;
    p->error_unterminated = false;
    if (input && parse_token_tid(input) == p->indent_tid) {
        p->continuation_indent_pos = complete_pos;
        p->held_indent_pos = complete_pos;
    }
    step_input(p, complete_pos, input, core_list_len(p->worklist));
}

static bool
run_parse(parse_pwz_parser_t *p)
{
    if (core_list_len(p->tops) > 0 && !get_token(p, 0)) {
        return true;
    }

    for (int32_t pos = 0; ; pos++) {
        recycle_released_cxts(p);
        core_list_t(pwz_zipper_t) tmp = p->worklist;
        p->worklist      = p->worklist_swap;
        p->worklist_swap = tmp;
        core_list_clear(p->worklist_swap);
        core_list_clear(p->tops);
        if (p->mems_since_collect >= (p->collect_threshold
                                          ? p->collect_threshold : 65536))
            collect_unreachable_mems(p);

        size_t wl_len = core_list_len(p->worklist);

        if (wl_len == 0) {
            int32_t trailing = pos;
            parse_token_info_t *tok = get_token(p, trailing);
            while (tok && parse_token_ignore_if_no_match(tok)) {
                tok = get_token(p, ++trailing);
            }
            if (!tok && p->longest_top && p->longest_end == pos) {
                core_list_clear(p->tops);
                core_list_push(p->tops, p->longest_top);
                return true;
            }
            p->error_pos = p->held_indent_pos >= 0
                && pos == p->held_indent_pos + 1 ? p->held_indent_pos : pos;
            return false;
        }

        int32_t complete_pos = pos + 1;

        bool have_next = get_token(p, complete_pos) != nullptr;
        parse_token_info_t *input = get_token(p, complete_pos);

        step_input(p, complete_pos, input, wl_len);

        if (p->held_pos >= 0 && p->held_pos == complete_pos - 1) {
            if (p->worklist_swap.len || p->tops.len) {
                drop_continuation(p);
            }
            else {
                resume_continuation(p, complete_pos, input);
            }
        }
        else if (p->held_pos >= 0 && p->held_pos != complete_pos) {
            drop_continuation(p);
        }

        if (!have_next) {
            if (core_list_len(p->tops) > 0) {
                return true;
            }
            p->error_pos = complete_pos;
            return false;
        }
    }
}

static parse_tree_t *
make_token_node(parse_token_info_t *tok)
{
    return core_tree_leaf(parse_nt_node_t, parse_token_info_ptr_t, tok);
}

static parse_tree_t *
make_epsilon_node(int32_t pos)
{
    parse_nt_node_t pn = {0};

    pn.name  = CORE_STRING_STATIC("\xce\xb5");
    pn.id    = PARSE_EMPTY_STRING;
    pn.start = pos;
    pn.end   = pos;

    return core_tree_node(parse_nt_node_t, parse_token_info_ptr_t, pn);
}

static parse_tree_t *
make_nt_node(parse_grammar_t *g, int64_t nt_id, int32_t rule_index,
             int32_t start, int32_t end)
{
    parse_nonterm_t *nt = parse_get_nonterm(g, nt_id);
    parse_nt_node_t  pn = {0};

    pn.name       = (nt && nt->name.data) ? nt->name : CORE_STRING_STATIC("?");
    pn.id         = nt_id;
    pn.rule_index = rule_index;
    pn.start      = start;
    pn.end        = end;

    return core_tree_node(parse_nt_node_t, parse_token_info_ptr_t, pn);
}

static parse_tree_t *
make_group_node(core_string_t name, int32_t start, int32_t end)
{
    parse_nt_node_t pn = {0};

    pn.name      = name.data ? name : CORE_STRING_STATIC("group");
    pn.id        = PARSE_GROUP_ID;
    pn.group_top = true;
    pn.start     = start;
    pn.end       = end;

    return core_tree_node(parse_nt_node_t, parse_token_info_ptr_t, pn);
}

static bool
same_group_node(parse_tree_t *a, parse_tree_t *b)
{
    if (!a || !b || core_tree_is_leaf(a) || core_tree_is_leaf(b)) {
        return false;
    }

    parse_nt_node_t an = core_tree_node_value(a);
    parse_nt_node_t bn = core_tree_node_value(b);

    if (!an.group_top || !bn.group_top) {
        return false;
    }

    if (an.name.u8_bytes != bn.name.u8_bytes) {
        return false;
    }

    if (an.name.u8_bytes == 0) {
        return true;
    }

    return an.name.data && bn.name.data
        && memcmp(an.name.data, bn.name.data,
                  (size_t)an.name.u8_bytes) == 0;
}

static void
add_child_flattening_same_group(parse_tree_t *parent,
                                parse_tree_t *child)
{
    if (!parent || !child) {
        return;
    }


    if (same_group_node(parent, child)) {
        if (parent->node->num_children == 0) {
            parse_tree_t **children = child->node->children;
            size_t count = child->node->num_children;
            core_free(parent->node->children);
            core_tree_replace_children(child, nullptr, 0);
            core_tree_replace_children(parent, children, count);
            parse_tree_free_node(child);
            return;
        }

        size_t nch = core_tree_num_children(child);

        for (size_t i = 0; i < nch; i++) {
            (void)core_tree_add_child(parent, core_tree_child(child, i));
        }

        parse_tree_free_node(child);
        return;
    }

    (void)core_tree_add_child(parent, child);
}

static bool
tree_is_identity_unit(parse_grammar_t *g, parse_tree_t *t)
{
    if (!t || core_tree_is_leaf(t)) return false;
    parse_nt_node_t  *node = &core_tree_node_value(t);
    parse_nonterm_t  *nt   = node->group_top || node->id < 0
        ? nullptr : parse_get_nonterm(g, node->id);
    if (!nt || node->rule_index < 0
        || (size_t)node->rule_index >= core_list_len(nt->rule_ids))
        return false;
    parse_rule_t *rule = parse_get_rule(g, nt->rule_ids.data[node->rule_index]);
    return rule && rule->identity_unit;
}

static parse_tree_t *
identity_chain_child(parse_grammar_t *g, parse_tree_t *tree,
                     parse_tree_t *kid)
{
    if (!tree_is_identity_unit(g, tree) || !tree_is_identity_unit(g, kid)
        || core_tree_num_children(kid) != 1) return kid;
    parse_tree_t *inner = core_tree_child(kid, 0);
    parse_tree_free_node(kid);
    return inner;
}

typedef struct {
    int32_t            pos;
    parse_pwz_parser_t *parser;
    bool               invalid;
} tree_convert_state_t;

static parse_rule_t *
exp_to_rule(parse_pwz_parser_t *p, pwz_exp_t *exp)
{
    if (!exp || exp->kind != PWZ_SEQ || exp->nt_id < 0 || exp->rule_ix < 0
        || exp->list_role != PWZ_LIST_NONE) {
        return nullptr;
    }

    parse_nonterm_t *nt = parse_get_nonterm(p->grammar, exp->nt_id);

    if (!nt || (size_t)exp->rule_ix >= core_list_len(nt->rule_ids)) {
        return nullptr;
    }

    return parse_get_rule(p->grammar, nt->rule_ids.data[exp->rule_ix]);
}

typedef struct {
    pwz_exp_t *exp;
    size_t     at;
    int32_t    cost;
    int32_t    from;
    int32_t    best_end;
    bool       entered;
} score_frame_t;

core_list_decl(score_frame_t);

static size_t
score_slot(pwz_exp_t *exp, int32_t pos, size_t cap)
{
    uint64_t h = (uint64_t)(uintptr_t)exp ^ ((uint64_t)(uint32_t)pos << 32);

    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;

    return (size_t)h & (cap - 1);
}

static void
score_index_place(parse_pwz_parser_t *p, size_t entry_ix)
{
    pwz_score_entry_t *e = &p->score_cache.data[entry_ix];
    size_t             i = score_slot(e->exp, e->pos, p->score_index_cap);

    while (p->score_index[i] >= 0) {
        i = (i + 1) & (p->score_index_cap - 1);
    }

    p->score_index[i] = (int32_t)entry_ix;
}

static void
score_index_grow(parse_pwz_parser_t *p)
{
    size_t cap = p->score_index_cap ? p->score_index_cap * 2 : 1024;

    core_free(p->score_index);
    p->score_index = core_alloc_array(int32_t, cap);
    p->score_index_cap = cap;

    for (size_t i = 0; i < cap; i++) {
        p->score_index[i] = -1;
    }

    for (size_t i = 0; i < p->score_cache.len; i++) {
        score_index_place(p, i);
    }
}

static void
score_cache_clear(parse_pwz_parser_t *p)
{
    core_list_clear(p->score_cache);

    for (size_t i = 0; i < p->score_index_cap; i++) {
        p->score_index[i] = -1;
    }
}

static void
score_cache_free(parse_pwz_parser_t *p)
{
    core_list_free(p->score_cache);
    core_free(p->score_index);
    p->score_index = nullptr;
    p->score_index_cap = 0;
}

static pwz_score_entry_t *
score_cache_entry(parse_pwz_parser_t *p, pwz_exp_t *exp, int32_t pos)
{
    if (p->score_index_cap == 0) {
        score_index_grow(p);
    }

    size_t i = score_slot(exp, pos, p->score_index_cap);

    while (p->score_index[i] >= 0) {
        pwz_score_entry_t *entry = &p->score_cache.data[p->score_index[i]];

        if (entry->exp == exp && entry->pos == pos) {
            return entry;
        }

        i = (i + 1) & (p->score_index_cap - 1);
    }

    core_list_push(p->score_cache,
                   ((pwz_score_entry_t){.exp = exp, .pos = pos}));

    size_t entry_ix = p->score_cache.len - 1;

    if ((entry_ix + 1) * 2 > p->score_index_cap) {
        score_index_grow(p);
    }
    else {
        p->score_index[i] = (int32_t)entry_ix;
    }

    return &p->score_cache.data[entry_ix];
}

static int32_t
score_exp(parse_pwz_parser_t *p, pwz_exp_t *root, int32_t *pos)
{
    core_list_t(score_frame_t) frames = core_list_new(score_frame_t);
    int32_t                   last   = 0;

    core_list_push(frames, ((score_frame_t){.exp = root}));

    while (frames.len > 0) {
        score_frame_t *f = &frames.data[frames.len - 1];

        if (!f->entered) {
            f->entered = true;

            if (!f->exp || f->exp == p->exp_bottom) {
                last = 0;
                frames.len--;
                continue;
            }

            *pos = live_pos(p, *pos);
            pwz_score_entry_t *cached = score_cache_entry(p, f->exp, *pos);

            if (cached->done) {
                last = cached->score;
                *pos = cached->end_pos;
                frames.len--;
                continue;
            }

            if (cached->visiting) {
                last = INT32_MAX / 4;
                frames.len--;
                continue;
            }

            cached->visiting = true;

            if (f->exp->kind == PWZ_TOK || f->exp->kind == PWZ_CLASS
                || f->exp->kind == PWZ_ANY || f->exp->kind == PWZ_SET) {
                (*pos)++;
                last = 0;
                cached = score_cache_entry(p, f->exp, *pos - 1);
                cached->score = 0;
                cached->end_pos = *pos;
                cached->visiting = false;
                cached->done = true;
                frames.len--;
                continue;
            }

            if (f->exp->kind == PWZ_SEQ) {
                f->from = *pos;
                parse_rule_t *rule = exp_to_rule(p, f->exp);

                if (rule && rule->disambig_predicate.data && p->disambig_cb) {
                    parse_token_info_t *tok = get_token(p, *pos);

                    if (p->disambig_cb(p->disambig_ud,
                                       rule->disambig_predicate, tok)) {
                        if (p->score_predicate_calls < UINT32_MAX)
                            p->score_predicate_calls++;
                        f->cost += rule->disambig_cost;
                    }
                    else if (p->score_predicate_calls < UINT32_MAX)
                        p->score_predicate_calls++;
                }
            }
            else {
                if (core_list_len(f->exp->alt.alts) == 0) {
                    last = 0;
                    cached = score_cache_entry(p, f->exp, *pos);
                    cached->score = 0;
                    cached->end_pos = *pos;
                    cached->visiting = false;
                    cached->done = true;
                    frames.len--;
                    continue;
                }
                f->cost     = INT32_MAX;
                f->from     = *pos;
                f->best_end = *pos;
            }
        }
        else if (f->exp->kind == PWZ_SEQ) {
            if (f->cost >= INT32_MAX / 4 || last >= INT32_MAX / 4
                || f->cost > INT32_MAX / 4 - last) {
                f->cost = INT32_MAX / 4;
            }
            else {
                f->cost += last;
            }
        }
        else if (last <= f->cost) {
            f->cost     = last;
            f->best_end = *pos;
        }

        size_t n = f->exp->kind == PWZ_SEQ ? (size_t)f->exp->nchildren
                                           : core_list_len(f->exp->alt.alts);

        if (f->at < n) {
            pwz_exp_t *kid;

            if (f->exp->kind == PWZ_SEQ) {
                kid = f->exp->seq.children[f->at];
            }
            else {
                kid  = f->exp->alt.alts.data[f->at];
                *pos = f->from;
            }
            f->at++;
            core_list_push(frames, ((score_frame_t){.exp = kid}));

            continue;
        }

        f = &frames.data[frames.len - 1];

        if (f->exp->kind == PWZ_ALT) {
            *pos = f->best_end;
        }

        parse_rule_t *rule = exp_to_rule(p, f->exp);
        for (size_t i = 0; rule && i < rule->annotations.len; i++) {
            parse_annotation_t *a = rule->annotations.data[i];
            if (!a || a->kind != PARSE_ANNOT_ADJACENT) {
                continue;
            }
            parse_token_info_t *first = get_token(p, f->from);
            parse_token_info_t *second = get_token(p, f->from + 1);
            if (!first || !second
                || parse_token_end_byte(first)
                       != parse_token_start_byte(second)) {
                f->cost = INT32_MAX / 4;
            }
            break;
        }
        last = f->cost;

        pwz_score_entry_t *cached = score_cache_entry(p, f->exp, f->from);
        cached->score = last;
        cached->end_pos = *pos;
        cached->visiting = false;
        cached->done = true;
        frames.len--;
    }

    core_list_free(frames);

    return last;
}

static pwz_exp_t *
resolve_alts(parse_pwz_parser_t *p, pwz_exp_t *exp, int32_t pos)
{
    while (exp && exp != p->exp_bottom && exp->kind == PWZ_ALT) {
        size_t nalts = core_list_len(exp->alt.alts);

        if (nalts == 0) {
            return nullptr;
        }

        size_t pick = nalts - 1;

        if ((p->disambig_cb || p->grammar->has_adjacency) && nalts > 0) {
            int32_t best = INT32_MAX;

            for (size_t i = 0; i < nalts; i++) {
                int32_t p2 = pos;
                int32_t c  = score_exp(p, exp->alt.alts.data[i], &p2);

                if (c <= best) {
                    if (i > 0 && c == best
                        && p->score_tie_count < UINT32_MAX)
                        p->score_tie_count++;
                    best = c;
                    pick = i;
                }
            }
            if (best >= INT32_MAX / 4) {
                return nullptr;
            }
        }

        exp = exp->alt.alts.data[pick];
    }

    return exp;
}

typedef struct {
    pwz_exp_t *exp;
    int32_t    start;
    int32_t    tail_start;
    int32_t    child_ix;
    size_t     kids_mark;
    bool       entered;
} conv_frame_t;

core_list_decl(conv_frame_t);

static bool
continues_same_group(parse_pwz_parser_t *p, core_list_t(conv_frame_t) *frames,
                     pwz_exp_t *e)
{
    if (e->kind != PWZ_SEQ || e->nt_id < 0 || frames->len < 2) {
        return false;
    }

    parse_nonterm_t *nt = parse_get_nonterm(p->grammar, e->nt_id);

    if (!nt || !nt->group_nt) {
        return false;
    }

    conv_frame_t *outer = &frames->data[frames->len - 2];

    return outer->exp && outer->exp->kind == PWZ_SEQ
        && outer->exp->nt_id == e->nt_id && outer->child_ix == 1;
}

static parse_tree_t *
convert_exp_to_tree(parse_pwz_parser_t *p, pwz_exp_t *exp,
                    tree_convert_state_t *st)
{
    core_list_t(conv_frame_t) frames = core_list_new(conv_frame_t);
    core_list_t(parse_tree_ptr_t) done
        = core_list_new(parse_tree_ptr_t);

    core_list_push(frames, ((conv_frame_t){.exp = exp}));

    while (frames.len > 0) {
        conv_frame_t *f = &frames.data[frames.len - 1];

        if (!f->entered) {
            f->entered = true;
            st->pos    = live_pos(p, st->pos);
            f->exp     = resolve_alts(p, f->exp, st->pos);

            pwz_exp_t *e = f->exp;

            parse_rule_t *rule = exp_to_rule(p, e);
            bool check_adjacent = false;
            for (size_t i = 0; rule && i < rule->annotations.len; i++) {
                parse_annotation_t *a = rule->annotations.data[i];
                if (a && a->kind == PARSE_ANNOT_ADJACENT) {
                    check_adjacent = true;
                    break;
                }
            }
            if (check_adjacent) {
                int32_t score_pos = st->pos;
                if (score_exp(p, e, &score_pos) >= INT32_MAX / 4) {
                    st->invalid = true;
                    core_list_push(done, make_epsilon_node(st->pos));
                    frames.len--;
                    continue;
                }
            }

            if (!e) {
                st->invalid = true;
                core_list_push(done, make_epsilon_node(st->pos));
                frames.len--;
                continue;
            }

            if (e == p->exp_bottom) {
                core_list_push(done, make_epsilon_node(st->pos));
                frames.len--;
                continue;
            }

            if (e->kind == PWZ_TOK || e->kind == PWZ_CLASS
                || e->kind == PWZ_ANY || e->kind == PWZ_SET) {
                st->pos = live_pos(p, st->pos);
                parse_token_info_t *tok = get_token(p, st->pos);

                if (!tok) {
                    core_list_push(done, make_epsilon_node(st->pos));
                }
                else {
                    core_list_push(done, make_token_node(tok));
                    st->pos++;
                }

                frames.len--;
                continue;
            }

            if (e->kind != PWZ_SEQ) {
                core_list_push(done, make_epsilon_node(st->pos));
                frames.len--;
                continue;
            }

            f->start = st->pos;

            if (e->nchildren == 0
                && (e->list_role == PWZ_LIST_STEP
                    || continues_same_group(p, &frames, e))) {
                frames.len--;
                continue;
            }

            if (e->nchildren == 0) {
                parse_tree_t *t;

                if (e->nt_id >= 0) {
                    parse_nonterm_t *nt = parse_get_nonterm(p->grammar, e->nt_id);

                    t = nt && nt->group_nt
                          ? make_group_node(e->seq.name, f->start, f->start)
                          : make_nt_node(p->grammar, e->nt_id, e->rule_ix,
                                         f->start, f->start);
                }
                else {
                    t = make_epsilon_node(f->start);
                }

                core_list_push(done, t);
                frames.len--;
                continue;
            }

            f->kids_mark = done.len;
        }

        pwz_exp_t *e = f->exp;

        if (f->child_ix < e->nchildren) {
            if (f->child_ix == 1) {
                f->tail_start = st->pos;
            }

            pwz_exp_t *child = e->seq.children[f->child_ix++];

            core_list_push(frames, ((conv_frame_t){.exp = child}));
            continue;
        }

        f = &frames.data[frames.len - 1];

        int32_t            start = f->start;
        int32_t            end   = st->pos;
        size_t             mark  = f->kids_mark;
        size_t             nch   = done.len - mark;
        parse_tree_t **kids  = done.data + mark;
        parse_tree_t  *tree;

        if (continues_same_group(p, &frames, e)) {
            frames.len--;
            continue;
        }

        if (e->list_role != PWZ_LIST_NONE) {
            size_t pending = nch - (size_t)(e->nchildren - 1);

            tree = make_nt_node(p->grammar, e->nt_id, e->rule_ix,
                                f->tail_start, end);

            for (size_t i = pending; i < nch; i++) {
                add_child_flattening_same_group(tree, kids[i]);
            }

            if (e->list_role == PWZ_LIST_END) {
                for (size_t i = pending; i-- > 0;) {
                    core_tree_add_child(kids[i], tree);
                    core_tree_node_value(kids[i]).end = end;
                    tree = kids[i];
                }

                pending = 0;
            }

            done.len = mark + pending;
            core_list_push(done, tree);
            frames.len--;
            continue;
        }

        if (e->nt_id >= 0) {
            parse_nonterm_t *nt = parse_get_nonterm(p->grammar, e->nt_id);

            tree = nt && nt->group_nt
                     ? make_group_node(e->seq.name, start, end)
                     : make_nt_node(p->grammar, e->nt_id, e->rule_ix, start,
                                    end);
            if (nch == 1)
                kids[0] = identity_chain_child(p->grammar, tree, kids[0]);
        }
        else if (nch == 1) {
            frames.len--;
            continue;
        }
        else {
            parse_nt_node_t pn = {0};

            pn.name  = e->seq.name.data ? e->seq.name
                                        : CORE_STRING_STATIC("?");
            pn.id    = e->nt_id;
            pn.start = start;
            pn.end   = end;
            tree = core_tree_node(parse_nt_node_t, parse_token_info_ptr_t, pn);
        }

        for (size_t i = 0; i < nch; i++) {
            add_child_flattening_same_group(tree, kids[i]);
        }

        done.len = mark;
        core_list_push(done, tree);
        frames.len--;
    }

    parse_tree_t *result = done.len > 0 ? done.data[done.len - 1]
                                            : make_epsilon_node(st->pos);

    core_list_free(frames);
    core_list_free(done);

    return st->invalid ? nullptr : result;
}

static parse_tree_t *
build_result_tree(parse_pwz_parser_t *p, pwz_exp_t *top_result)
{
    tree_convert_state_t st = {.pos = 0, .parser = p};

    score_cache_clear(p);

    return convert_exp_to_tree(p, top_result, &st);
}

#define PWZ_TREES_MANY INT32_MAX
#define PWZ_COUNTING   ((void *)(intptr_t)-1)

typedef struct {
    pwz_exp_t *exp;
    size_t     at;
    int64_t    acc;
    bool       entered;
} count_frame_t;

core_list_decl(count_frame_t);

static bool
counts_by_alt(const pwz_exp_t *e)
{
    return e->kind == PWZ_ALT && e->nt_id == -1;
}

static size_t
count_arity(const pwz_exp_t *e)
{
    return counts_by_alt(e) ? core_list_len(e->alt.alts)
                            : (size_t)e->nchildren;
}

static pwz_exp_t *
count_child(const pwz_exp_t *e, size_t i)
{
    return counts_by_alt(e) ? e->alt.alts.data[i] : e->seq.children[i];
}

static int32_t
count_trees_in_exp(pwz_exp_t *root)
{
    if (!root) {
        return 1;
    }

    core_dict_t memo;

    core_dict_init(&memo, core_hash_word, nullptr);

    core_list_t(count_frame_t) frames = core_list_new(count_frame_t);
    int64_t                   last   = 1;

    core_list_push(frames, ((count_frame_t){.exp = root}));

    while (frames.len > 0) {
        count_frame_t *f = &frames.data[frames.len - 1];

        if (!f->entered) {
            bool  found = false;
            void *hit   = core_dict_get(&memo, f->exp, &found);

            if (found) {
                last = hit == PWZ_COUNTING ? PWZ_TREES_MANY
                                           : (int64_t)(intptr_t)hit;
                frames.len--;
                continue;
            }

            f->entered = true;

            if (!counts_by_alt(f->exp) && f->exp->kind != PWZ_SEQ) {
                core_dict_put(&memo, f->exp, (void *)(intptr_t)1);
                last = 1;
                frames.len--;
                continue;
            }

            core_dict_put(&memo, f->exp, PWZ_COUNTING);
            f->acc = counts_by_alt(f->exp) ? 0 : 1;
        }
        else {
            f->acc = counts_by_alt(f->exp) ? f->acc + last : f->acc * last;

            if (f->acc > PWZ_TREES_MANY) {
                f->acc = PWZ_TREES_MANY;
            }
        }

        if (f->at < count_arity(f->exp)) {
            pwz_exp_t *kid = count_child(f->exp, f->at++);

            core_list_push(frames, ((count_frame_t){.exp = kid}));
            continue;
        }

        f    = &frames.data[frames.len - 1];
        last = f->acc;
        core_dict_put(&memo, f->exp, (void *)(intptr_t)f->acc);
        frames.len--;
    }

    core_list_free(frames);
    core_dict_free(&memo);

    return (int32_t)last;
}

static pwz_exp_t *
find_top_ambiguity(pwz_exp_t *exp)
{
    while (exp) {
        if (exp->kind == PWZ_ALT && exp->nt_id == -1) {
            return exp;
        }

        if (exp->kind != PWZ_SEQ || exp->nt_id != -1
            || exp->nchildren != 1) {
            return nullptr;
        }
        exp = exp->seq.children[0];
    }

    return nullptr;
}

static void
enumerate_trees(parse_pwz_parser_t   *p,
                pwz_exp_t           *top_result,
                parse_tree_ptr_t **out,
                int32_t              *out_count)
{
    int32_t total = count_trees_in_exp(top_result);

    if (total <= 1) {
        *out_count = 1;
        *out       = core_alloc_array(parse_tree_ptr_t, 1);
        (*out)[0]  = build_result_tree(p, top_result);
        return;
    }

    pwz_exp_t *amb = find_top_ambiguity(top_result);

    if (amb) {
        size_t nalts = core_list_len(amb->alt.alts);

        *out_count = (int32_t)nalts;
        *out       = core_alloc_array(parse_tree_ptr_t, nalts);

        for (size_t i = 0; i < nalts; i++) {
            tree_convert_state_t st = {.pos = 0, .parser = p};
            (*out)[i] = convert_exp_to_tree(p, amb->alt.alts.data[i], &st);
        }

        return;
    }

    *out_count = 1;
    *out       = core_alloc_array(parse_tree_ptr_t, 1);
    (*out)[0]  = build_result_tree(p, top_result);
}

static int32_t
layout_tid(parse_grammar_t *g, core_string_t name)
{
    int64_t tid = name.data ? parse_terminal_id(g, name) : 0;

    return tid ? (int32_t)tid : -1;
}

static core_list_t(int32_t)
layout_tids(parse_grammar_t *g, parse_annotation_t *a)
{
    core_list_t(int32_t) tids = core_list_new(int32_t);

    for (size_t i = 0; a && i < core_list_len(a->tags); i++) {
        int32_t tid = layout_tid(g, a->tags.data[i]);
        if (tid >= 0) {
            core_list_push(tids, tid);
        }
    }

    return tids;
}

static void
load_statement_ends(parse_pwz_parser_t *p, parse_grammar_t *g)
{
    p->statement_ends          = (core_list_t(int32_t)){0};
    p->continue_after          = (core_list_t(int32_t)){0};
    p->skipped                 = core_list_new(int32_t);
    p->held                    = core_list_new(pwz_zipper_t);
    p->held_pos                = -1;
    p->held_indent_pos         = -1;
    p->end_run                 = PWZ_RUN_NONE;
    p->newline_tid             = -1;
    p->indent_tid              = -1;
    p->dedent_tid              = -1;
    p->continuation_indent_pos = -1;
    p->continuation_indents    = 0;

    parse_annotation_t *ends = parse_grammar_find_annot(
        g, PARSE_ANNOT_STATEMENT_END);

    if (!ends || !core_list_len(ends->tags)) {
        return;
    }

    p->statement_ends = layout_tids(g, ends);
    p->continue_after = layout_tids(
        g, parse_grammar_find_annot(g, PARSE_ANNOT_CONTINUE_AFTER));

    const scan_lex_spec_t *spec = scan_grammar_lex(g);

    if (spec) {
        p->newline_tid = layout_tid(g, spec->tok_newline);
        p->indent_tid  = layout_tid(g, spec->tok_indent);
        p->dedent_tid  = layout_tid(g, spec->tok_dedent);
    }
}

parse_pwz_parser_t *
parse_pwz_new_for_start(parse_grammar_t *g, parse_nt_id_t start)
{
    if (!g || start < 0 || (size_t)start >= core_list_len(g->nt_list)) {
        return nullptr;
    }

    parse_grammar_finalize(g);

    parse_pwz_parser_t *p = core_alloc(parse_pwz_parser_t);

    p->grammar       = g;
    p->start_nt      = start;
    core_arena_init(&p->parse_arena, CORE_ARENA_DEFAULT_BLOCK_SIZE);
    p->all_exps      = core_list_new(pwz_exp_ptr_t);
    p->worklist      = core_list_new(pwz_zipper_t);
    p->worklist_swap = core_list_new(pwz_zipper_t);
    p->tops          = core_list_new(pwz_exp_ptr_t);
    p->score_cache   = core_list_new(pwz_score_entry_t);
    p->disambig_cb   = nullptr;
    p->disambig_ud   = nullptr;
    p->stream_first  = 0;
    p->stream_end    = INT32_MAX;
    p->expected_pos  = -1;
    p->expected      = core_list_new(parse_pwz_expected_t);

    p->exp_bottom       = core_alloc(pwz_exp_t);
    p->exp_bottom->kind = PWZ_SEQ;

    p->mem_bottom            = core_alloc(pwz_mem_t);
    p->mem_bottom->start_pos = PWZ_POS_BOTTOM;
    p->mem_bottom->end_pos   = PWZ_POS_BOTTOM;

    build_exp_graph(p, g, start);
    load_statement_ends(p, g);

    return p;
}

void
parse_pwz_free(parse_pwz_parser_t *p)
{
    if (!p) {
        return;
    }

    reset_memos(p);
    core_arena_free(&p->parse_arena);

    size_t num_exps = core_list_len(p->all_exps);

    for (size_t i = 0; i < num_exps; i++) {
        pwz_exp_t *e = p->all_exps.data[i];

        if (e->kind == PWZ_SEQ && e->seq.children) {
            core_free(e->seq.children);
        }
        else if (e->kind == PWZ_ALT) {
            core_list_free(e->alt.alts);
        }

        core_free(e);
    }

    core_list_free(p->all_exps);
    core_list_free(p->expected);
    core_list_free(p->statement_ends);
    core_list_free(p->continue_after);
    core_list_free(p->skipped);
    core_list_free(p->held);
    core_free(p->nt_exps);
    core_free(p->exp_bottom);
    core_free(p->mem_bottom);
    core_list_free(p->worklist);
    core_list_free(p->free_cxts);
    core_list_free(p->pending_cxts);
    core_list_free(p->arena_mems);
    core_list_free(p->mark_stack);
    core_list_free(p->worklist_swap);
    core_list_free(p->tops);
    core_list_free(p->steps);
    score_cache_free(p);

    if (p->result_trees.data) {
        for (size_t i = 0; i < p->result_trees.len; i++) {
            if (p->result_trees.data[i] == p->result_tree) {
                p->result_tree = nullptr;
            }
            parse_tree_free(p->result_trees.data[i]);
        }
        core_array_free(p->result_trees);
    }
    parse_tree_free(p->result_tree);

    core_free(p);
}

void
parse_pwz_release_parse_state(parse_pwz_parser_t *p)
{
    if (!p) {
        return;
    }

    reset_memos(p);
    core_arena_free(&p->parse_arena);
    p->free_mems = nullptr;
    drop_released_cxts(p);
    core_list_free(p->free_cxts);
    core_list_free(p->pending_cxts);
    core_list_free(p->arena_mems);
    core_list_free(p->mark_stack);
    core_list_free(p->worklist);
    core_list_free(p->worklist_swap);
    core_list_free(p->tops);
    core_list_free(p->steps);
    score_cache_free(p);
}

void
parse_pwz_reset(parse_pwz_parser_t *p)
{
    if (!p) {
        return;
    }
    if (p->result_trees.data) {
        for (size_t i = 0; i < p->result_trees.len; i++) {
            if (p->result_trees.data[i] == p->result_tree) {
                p->result_tree = nullptr;
            }
            parse_tree_free(p->result_trees.data[i]);
        }
        core_array_free(p->result_trees);
    }
    parse_tree_free(p->result_tree);

    reset_memos(p);
    core_arena_reset(&p->parse_arena);
    p->free_mems = nullptr;
    drop_released_cxts(p);
    core_list_clear(p->worklist);
    core_list_clear(p->worklist_swap);
    core_list_clear(p->tops);
    p->stream       = nullptr;
    p->result_tree  = nullptr;
    p->result_trees = (parse_tree_array_t){0};
    p->error_pos    = -1;
    p->error_unterminated = false;
    p->error_commented = false;
    p->continuation_indent_pos = -1;
    p->continuation_indents = 0;
    p->end_run = PWZ_RUN_NONE;
    core_list_clear(p->skipped);
    core_list_clear(p->held);
    p->held_pos = -1;
    p->held_indent_pos = -1;
    p->score_predicate_calls = 0;
    p->score_tie_count = 0;
}

static bool
parse_window(parse_pwz_parser_t *p, parse_token_stream_t *ts, int32_t first,
             int32_t end)
{
    parse_pwz_reset(p);

    p->stream       = ts;
    p->stream_first = first;
    p->stream_end   = end;

    scan_stream_get(ts, first);

    init_parse(p);
    bool ok = run_parse(p);

    if (ok && core_list_len(p->tops) > 0) {
        p->result_tree = build_result_tree(p, p->tops.data[0]);
        return p->result_tree != nullptr;
    }

    return false;
}

bool
parse_pwz_parse(parse_pwz_parser_t   *p,
               parse_token_stream_t *ts)
{
    return parse_window(p, ts, 0, INT32_MAX);
}

bool
parse_pwz_parse_range(parse_pwz_parser_t   *p,
                      parse_token_stream_t *ts,
                      int32_t               first,
                      int32_t               end)
{
    if (!p || !ts || first < 0 || end < first) {
        return false;
    }
    return parse_window(p, ts, first, end);
}

bool
parse_pwz_collect_expected(parse_pwz_parser_t *p, int32_t pos)
{
    if (!p || !p->stream || pos < p->stream_first) {
        return false;
    }
    core_list_clear(p->expected);
    parse_token_stream_t *ts = p->stream;
    int32_t first = p->stream_first;
    int32_t end = p->stream_end;
    parse_pwz_reset(p);
    p->stream       = ts;
    p->stream_first = first;
    p->stream_end   = end;
    p->expected_pos = pos - first;
    scan_stream_get(ts, first);
    init_parse(p);
    run_parse(p);
    p->expected_pos = -1;
    return core_list_len(p->expected) > 0;
}

size_t
parse_pwz_expected_count(const parse_pwz_parser_t *p)
{
    return p ? core_list_len(p->expected) : 0;
}

const parse_pwz_expected_t *
parse_pwz_expected_at(const parse_pwz_parser_t *p, size_t index)
{
    return p && index < core_list_len(p->expected)
        ? &p->expected.data[index] : nullptr;
}

bool
parse_pwz_parse_prefix(parse_pwz_parser_t   *p,
                     parse_token_stream_t *ts,
                     int32_t            *ntokens)
{
    parse_pwz_reset(p);

    p->stream       = ts;
    p->stream_first = 0;
    p->stream_end   = INT32_MAX;

    scan_stream_get(ts, 0);

    init_parse(p);
    run_parse(p);

    if (!p->longest_top) {
        return false;
    }

    p->result_tree = build_result_tree(p, p->longest_top);
    if (!p->result_tree) {
        return false;
    }

    if (ntokens) {
        *ntokens = p->longest_end;
    }

    return true;
}

parse_tree_t *
parse_pwz_get_tree(parse_pwz_parser_t *p)
{
    return p->result_tree;
}

int32_t
parse_pwz_error_pos(parse_pwz_parser_t *p)
{
    if (!p || p->error_pos < 0) {
        return -1;
    }
    return p->error_pos + p->stream_first;
}

bool
parse_pwz_error_unterminated(parse_pwz_parser_t *p)
{
    return p && p->error_unterminated;
}

bool
parse_pwz_error_commented(parse_pwz_parser_t *p)
{
    return p && p->error_commented;
}

void
parse_pwz_set_disambiguator(parse_pwz_parser_t    *p,
                          parse_pwz_disambig_fn  cb,
                          void                *ud)
{
    if (!p) {
        return;
    }

    p->disambig_cb = cb;
    p->disambig_ud = ud;
}

parse_tree_array_t
parse_pwz_get_trees(parse_pwz_parser_t *p)
{
    if (p->result_trees.data) {
        return p->result_trees;
    }

    size_t ntops = core_list_len(p->tops);

    if (ntops == 0) {
        p->result_trees = core_array_new(parse_tree_ptr_t, 0);
        return p->result_trees;
    }

    if (ntops == 1) {
        int32_t total = count_trees_in_exp(p->tops.data[0]);
        if (total <= 1) {
            p->result_trees = core_array_new(parse_tree_ptr_t, 1);
            core_array_set(p->result_trees, 0,
                           p->result_tree
                               ? p->result_tree
                               : build_result_tree(p, p->tops.data[0]));
            p->result_tree = p->result_trees.data[0];
            return p->result_trees;
        }

        parse_tree_free(p->result_tree);
        p->result_tree = nullptr;
        parse_tree_ptr_t *raw_trees = nullptr;
        int32_t                raw_count = 0;

        enumerate_trees(p, p->tops.data[0], &raw_trees, &raw_count);

        p->result_trees = core_array_new(parse_tree_ptr_t, raw_count);

        for (int32_t i = 0; i < raw_count; i++) {
            core_array_set(p->result_trees, i, raw_trees[i]);
        }

        if (raw_trees) {
            core_free(raw_trees);
        }
    }
    else {
        parse_tree_free(p->result_tree);
        p->result_tree = nullptr;
        p->result_trees = core_array_new(parse_tree_ptr_t, (int32_t)ntops);

        for (size_t i = 0; i < ntops; i++) {
            core_array_set(p->result_trees, (int32_t)i,
                           build_result_tree(p, p->tops.data[i]));
        }
    }

    return p->result_trees;
}

parse_forest_t
parse_pwz_get_forest(parse_pwz_parser_t *p)
{
    parse_tree_array_t trees = parse_pwz_get_trees(p);
    parse_forest_t forest = parse_forest_new(p->grammar, trees);
    p->result_tree = nullptr;
    p->result_trees = (parse_tree_array_t){0};
    return forest;
}

parse_forest_t
parse_pwz_parse_grammar(parse_grammar_t      *g,
                        parse_nt_id_t         start,
                        parse_token_stream_t *ts)
{
    return parse_pwz_parse_grammar_at(g, start, ts, nullptr);
}

parse_forest_t
parse_pwz_parse_grammar_at(parse_grammar_t      *g,
                           parse_nt_id_t         start,
                           parse_token_stream_t *ts,
                           int32_t              *error_pos)
{
    parse_pwz_parser_t *p = parse_pwz_new_for_start(g, start);

    if (!p) {
        if (error_pos) {
            *error_pos = -1;
        }
        return parse_forest_empty(g);
    }

    bool               ok = parse_pwz_parse(p, ts);

    if (error_pos) {
        *error_pos = parse_pwz_error_pos(p);
    }

    if (!ok) {
        parse_pwz_free(p);
        return parse_forest_empty(g);
    }

    parse_forest_t forest = parse_pwz_get_forest(p);

    parse_pwz_free(p);

    return forest;
}
