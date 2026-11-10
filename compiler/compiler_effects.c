#include "compiler_effects.h"
#include "cached_body.h"

#include "ir/sites.h"
#include "lib/dict.h"

static const n00bb_effect_name_t effect_names[] = {
#define IR_EFFECT_NAME(name, bits) \
    {CORE_STRING_STATIC(#name), (uint64_t)(bits), false},
#define IR_EFFECT_GROUP(name, bits) \
    {CORE_STRING_STATIC(#name), (uint64_t)(bits), true},
#include "ir/effect_names.def"
};

size_t
n00bb_effect_name_count(void)
{
    return sizeof(effect_names) / sizeof(effect_names[0]);
}

const n00bb_effect_name_t *
n00bb_effect_name_at(size_t index)
{
    return index < n00bb_effect_name_count() ? &effect_names[index]
                                              : nullptr;
}

const n00bb_effect_name_t *
n00bb_effect_named(core_string_t name)
{
    for (size_t i = 0; i < n00bb_effect_name_count(); i++)
        if (core_string_eq(effect_names[i].name, name))
            return &effect_names[i];
    return nullptr;
}

uint64_t
n00bb_named_effect_bits(void)
{
    uint64_t bits = 0;
    for (size_t i = 0; i < n00bb_effect_name_count(); i++)
        if (!effect_names[i].group) bits |= effect_names[i].bits;
    return bits;
}

static bool
reaches_element(const ir_func_t *function, const ir_spath_t *path,
                uint32_t below)
{
    for (uint32_t depth = 1; depth < below; depth++) {
        th_type *type = ir_summary_path_type(function, path, depth);
        if (type && th_kind_of(th_resolve(type)) == TH_VAR) return true;
    }
    return false;
}

static uint8_t
container_access(const ir_func_t *function, const ir_spath_t *path)
{
    if (!reaches_element(function, path, path->depth)) return path->flags;
    return path->flags & IR_SPATH_ACCESS ? IR_SPATH_READ : 0;
}

static bool
lends_through(const ir_func_t *function, const ir_summary_t *summary,
              ir_sflow_table_t table, uint32_t param)
{
    for (uint32_t s = 0; s < ir_summary_flow_source_count(summary, table);
         s++) {
        const ir_spath_t *source = ir_summary_flow_source(summary, table, s);
        if (source->root != IR_SPLACE_PARAM || source->param != param
            || reaches_element(function, source, source->depth + 1))
            continue;
        for (uint32_t f = 0; f < ir_summary_flow_count(summary, table); f++)
            if (ir_summary_flow_has(summary, table, f, s, IR_SFLOW_ALIAS))
                return true;
    }
    return false;
}

uint8_t
n00bb_parameter_role(const ir_func_t *function, const ir_summary_t *summary,
                     uint32_t param)
{
    if (!summary) return N00BB_ROLE_READS | N00BB_ROLE_WRITES
                       | N00BB_ROLE_LENDS;
    uint8_t role = 0;
    for (uint32_t i = 0; i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        bool rooted = path->root == IR_SPLACE_PARAM && path->param == param;
        bool unknown = path->root == IR_SPLACE_UNKNOWN
            && !(path->flags & IR_SPATH_TYPED);
        if (!rooted && !unknown) continue;
        uint8_t flags = rooted ? container_access(function, path)
                               : path->flags;
        if (flags & (IR_SPATH_READ | IR_SPATH_READS_ENTRY))
            role |= N00BB_ROLE_READS;
        if (flags & IR_SPATH_WRITE) role |= N00BB_ROLE_WRITES;
    }
    if ((ir_summary_param_escape(summary, param) & IR_SESCAPE_ADDRESS)
        || lends_through(function, summary, IR_SFLOW_RESULTS, param)
        || lends_through(function, summary, IR_SFLOW_STORES, param))
        role |= N00BB_ROLE_LENDS;
    return role;
}

static const core_string_t query_words[] = {
    [N00BB_QUERY_NONE]           = CORE_STRING_STATIC(""),
    [N00BB_QUERY_MAY_EFFECT]     = CORE_STRING_STATIC("may_effect"),
    [N00BB_QUERY_FORBIDS_EFFECT] = CORE_STRING_STATIC("forbids_effect"),
    [N00BB_QUERY_HAS_EFFECT]     = CORE_STRING_STATIC("has_effect"),
    [N00BB_QUERY_MAY_READ]       = CORE_STRING_STATIC("may_read"),
    [N00BB_QUERY_MAY_WRITE]      = CORE_STRING_STATIC("may_write"),
    [N00BB_QUERY_MAY_LEND]       = CORE_STRING_STATIC("may_lend"),
    [N00BB_QUERY_MAY_CARRY]      = CORE_STRING_STATIC("may_carry"),
};

n00bb_query_t
n00bb_query_named(core_string_t word)
{
    for (size_t q = 1; q < sizeof(query_words) / sizeof(query_words[0]); q++)
        if (core_string_eq(query_words[q], word)) return (n00bb_query_t)q;
    return N00BB_QUERY_NONE;
}

core_string_t
n00bb_query_word(n00bb_query_t query)
{
    return query_words[query];
}

bool
n00bb_query_about_effect(n00bb_query_t query)
{
    return query == N00BB_QUERY_MAY_EFFECT
        || query == N00BB_QUERY_FORBIDS_EFFECT
        || query == N00BB_QUERY_HAS_EFFECT;
}

int64_t
n00bb_parameter_named(const ir_func_t *function, core_string_t name)
{
    for (uint32_t p = 0; function && p < function->n_params; p++)
        if (function->params[p]
            && core_string_eq(function->params[p]->name, name))
            return p;
    return -1;
}

typedef struct {
    ir_summaries_t            *summaries;
    uint64_t                   bits;
    const n00bb_user_effect_t *user;
    core_dict_t                known;
    bool                       callees_must;
    const ir_func_t           *current;
} must_t;

static bool must_body(must_t *must, ir_func_t *function);

static void
must_callee(void *context, const ir_func_t *target,
            const ir_summary_t *summary)
{
    must_t *must = context;
    ir_func_t *body = n00bb_cached_body((ir_func_t *)target);
    const n00bb_user_effect_t *user = must->user;
    if (user && n00bb_function_listed(&user->handlers, body)) {
        must->callees_must = false;
        return;
    }
    bool does = user && n00bb_function_listed(&user->sources, body) ? true
        : user && !n00bb_effect_crosses(user, must->current,
                                        body ? body : target) ? false
        : body && body->n_blocks ? must_body(must, body)
        : n00bb_is_callback_bound(target)
        ? (user && n00bb_bound_names(target, N00BB_QUERY_HAS_EFFECT,
                                     user->qualified))
            || (n00bb_bound_bits(target, N00BB_QUERY_HAS_EFFECT)
                & must->bits) != 0
        : summary && (ir_summary_effect_bits(summary) & must->bits);
    must->callees_must = must->callees_must && does;
}

static uint8_t
must_step(void *context, const ir_instr_t *in, ir_path_outcome_t outcome)
{
    (void)outcome;
    must_t *must = context;
    bool call = in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE;
    if (!call) {
        bool allocates = in->op == IR_OP_ALLOC || in->op == IR_OP_ARRAY_NEW;
        return allocates && (must->bits & IR_SUMMARY_MAY_ALLOCATE)
            ? IR_PATH_PASSED : IR_PATH_UNPASSED;
    }
    bool outer = must->callees_must;
    must->callees_must = true;
    bool known = ir_summary_call_callees(must->summaries, in, must_callee,
                                         must);
    bool passed = known && must->callees_must;
    must->callees_must = outer;
    return passed ? IR_PATH_PASSED : IR_PATH_UNPASSED;
}

static bool
must_body(must_t *must, ir_func_t *function)
{
    enum { MUST_NOT = 1, MUST_DOES = 2 };
    bool found = false;
    uintptr_t known = (uintptr_t)core_dict_get(&must->known, function,
                                               &found);
    if (found) return known == MUST_DOES;
    core_dict_put(&must->known, function, (void *)(uintptr_t)MUST_NOT);
    ir_path_exits_t exits;
    const ir_func_t *outer = must->current;
    must->current = function;
    bool does = ir_paths_flow(function, IR_PATH_UNPASSED, must_step, must,
                              &exits)
        && !(exits.returned & IR_PATH_UNPASSED)
        && !(exits.failed & IR_PATH_UNPASSED);
    must->current = outer;
    core_dict_put(&must->known, function,
                  (void *)(uintptr_t)(does ? MUST_DOES : MUST_NOT));
    return does;
}

bool
n00bb_must_effect(ir_summaries_t *summaries, ir_func_t *function,
                  uint64_t bits, const n00bb_user_effect_t *user)
{
    if (user && n00bb_function_listed(&user->sources, function)) return true;
    must_t must = {.summaries = summaries, .bits = bits, .user = user};
    core_dict_init(&must.known, nullptr, nullptr);
    bool does = must_body(&must, function);
    core_dict_free(&must.known);
    return does;
}

const ir_instr_t *
n00bb_must_skip(ir_summaries_t *summaries, ir_func_t *function, uint64_t bits,
                const n00bb_user_effect_t *user)
{
    if (!function || !function->n_blocks
        || (user && n00bb_function_listed(&user->sources, function)))
        return nullptr;
    must_t must = {.summaries = summaries, .bits = bits, .user = user,
                   .current = function};
    core_dict_init(&must.known, nullptr, nullptr);
    const ir_instr_t *exit = nullptr;
    for (uint32_t b = 0; !exit && b < function->n_blocks; b++) {
        ir_instr_t *terminator = ir_block_terminator(function->blocks[b]);
        if (terminator
            && (terminator->op == IR_OP_RETURN || terminator->op == IR_OP_FAIL)
            && (ir_paths_state(function, IR_PATH_UNPASSED, must_step, &must,
                               terminator) & IR_PATH_UNPASSED))
            exit = terminator;
    }
    core_dict_free(&must.known);
    return exit;
}

bool
n00bb_effect_scope_named(core_string_t word, n00bb_effect_scope_t *out)
{
    const core_string_t words[] = {
        [N00BB_EFFECT_SCOPE_THREAD] = CORE_STRING_STATIC("thread"),
        [N00BB_EFFECT_SCOPE_PACKAGE] = CORE_STRING_STATIC("package"),
        [N00BB_EFFECT_SCOPE_CALLER] = CORE_STRING_STATIC("caller"),
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(*words); i++)
        if (core_string_eq(word, words[i])) {
            *out = (n00bb_effect_scope_t)i;
            return true;
        }
    return false;
}

bool
n00bb_clause_matches(const n00bb_effect_clause_t *clause, ir_func_t *body)
{
    return body && (clause->function == body
        || (clause->native.u8_bytes && !body->n_blocks
            && core_string_eq(clause->native, n00bb_native_symbol(body))));
}

bool
n00bb_function_listed(const core_list_t(n00bb_effect_clause_t) *clauses,
                      ir_func_t *function)
{
    ir_func_t *body = n00bb_cached_body(function);
    for (size_t i = 0; body && i < clauses->len; i++)
        if (n00bb_clause_matches(&clauses->data[i], body)) return true;
    return false;
}

bool
n00bb_query_answer(ir_summaries_t *summaries, ir_func_t *subject,
                   n00bb_query_t query, core_string_t name,
                   const n00bb_user_effect_t *user)
{
    ir_summary_t *summary = subject ? ir_summary_for(summaries, subject)
                                    : nullptr;
    if (!summary) return query != N00BB_QUERY_NONE
        && query != N00BB_QUERY_FORBIDS_EFFECT
        && query != N00BB_QUERY_HAS_EFFECT;
    const n00bb_effect_name_t *effect = n00bb_effect_named(name);
    uint32_t flags = ir_summary_flags(summary);
    bool may = effect
        ? (flags & IR_SUMMARY_CALLS_UNKNOWN)
            || (ir_summary_effect_bits(summary) & effect->bits)
        : (flags & IR_SUMMARY_CALLS_UNKNOWN)
            || ir_summary_has_effect(summary, name)
            || (user && n00bb_function_listed(&user->sources, subject));
    int64_t param = n00bb_parameter_named(subject, name);
    uint8_t role = param >= 0
        ? n00bb_parameter_role(subject, summary, (uint32_t)param) : 0;
    switch (query) {
    case N00BB_QUERY_MAY_EFFECT:
        return may;
    case N00BB_QUERY_FORBIDS_EFFECT:
        return !may;
    case N00BB_QUERY_HAS_EFFECT:
        if (!effect)
            return user && n00bb_must_effect(summaries, subject, 0, user);
        if (effect->bits == IR_SUMMARY_MAY_DIVERGE)
            return (flags & IR_SUMMARY_DIVERGES)
                || (user && n00bb_must_effect(summaries, subject,
                                              effect->bits, user));
        return n00bb_must_effect(summaries, subject, effect->bits, user);
    case N00BB_QUERY_MAY_READ:
        return (role & N00BB_ROLE_READS) != 0;
    case N00BB_QUERY_MAY_WRITE:
        return (role & N00BB_ROLE_WRITES) != 0;
    case N00BB_QUERY_MAY_LEND:
        return (role & N00BB_ROLE_LENDS) != 0;
    case N00BB_QUERY_MAY_CARRY:
    case N00BB_QUERY_NONE:
        break;
    }
    return false;
}

bool
n00bb_bound_parse(core_string_t tag, n00bb_bound_t *out)
{
    int64_t first = core_string_find_byte(tag, ':');
    if (first <= 0) return false;
    core_string_t rest = core_string_slice(tag, (size_t)first + 1,
                                           tag.u8_bytes - (size_t)first - 1);
    int64_t second = core_string_find_byte(rest, ':');
    if (second <= 0) return false;
    uint64_t param = 0;
    size_t consumed = 0;
    if (!core_string_parse_u64(core_string_slice(rest, 0, (size_t)second), 10,
                               &param, &consumed)
        || consumed != (size_t)second || param > UINT32_MAX)
        return false;
    *out = (n00bb_bound_t){
        .query  = n00bb_query_named(core_string_slice(tag, 0,
                                                      (size_t)first)),
        .param  = (uint32_t)param,
        .effect = core_string_slice(rest, (size_t)second + 1,
                                    rest.u8_bytes - (size_t)second - 1),
    };
    return (out->query == N00BB_QUERY_FORBIDS_EFFECT
            || out->query == N00BB_QUERY_HAS_EFFECT)
        && out->effect.u8_bytes;
}

void
n00bb_bound_tag(core_buffer_t *out, n00bb_query_t query, uint32_t param,
                core_string_t effect)
{
    core_buffer_append_string(out, n00bb_query_word(query));
    core_buffer_printf(out, ":%u:", param);
    core_buffer_append_string(out, effect);
}

bool
n00bb_bound_names(const ir_func_t *function, n00bb_query_t query,
                  core_string_t effect)
{
    for (uint32_t e = 0;; e++) {
        core_string_t tag = ir_func_effect_at(function, e);
        if (!tag.u8_bytes) return false;
        n00bb_bound_t bound;
        if (n00bb_bound_parse(tag, &bound) && bound.query == query
            && core_string_eq(bound.effect, effect)) return true;
    }
}

uint64_t
n00bb_bound_bits(const ir_func_t *function, n00bb_query_t query)
{
    uint64_t bits = 0;
    for (uint32_t e = 0;; e++) {
        core_string_t tag = ir_func_effect_at(function, e);
        if (!tag.u8_bytes) break;
        n00bb_bound_t bound;
        const n00bb_effect_name_t *effect = n00bb_bound_parse(tag, &bound)
            && bound.query == query ? n00bb_effect_named(bound.effect)
                                    : nullptr;
        if (effect) bits |= effect->bits;
    }
    return bits;
}
