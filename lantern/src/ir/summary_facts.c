#include "ir/summary_facts.h"

#include "ir/analyses.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/list.h"

typedef const ir_func_t *facts_func_ptr_t;
core_list_decl(facts_func_ptr_t);
core_list_decl(ir_splace_t);

typedef enum : uint8_t {
    REL_FUNCTION,
    REL_FLAG,
    REL_TRAP_KIND,
    REL_ACCESS,
    REL_PLACE_KIND,
    REL_PARAM_PLACE,
    REL_CALL,
    REL_CALL_TARGET,
    REL_CALL_UNKNOWN,
    REL_OUT_ARGUMENT,
    REL_DISCARDED,
    REL_ADDRESSABLE_ARGUMENT,
    REL_LOAD,
    REL_STORE,
    REL_RESULT_EFFECT,
    REL_SHARED_PLACE,
    REL_VISIBLE_WRITE,
    REL_SIDE_EFFECT,
    REL_EFFECTFUL_CALL,
    REL_IGNORED_RESULT,
    REL_COUNT,
} relation_t;

static const struct {
    core_string_t name;
    int32_t       arity;
} relations[REL_COUNT] = {
    [REL_FUNCTION]             = {CORE_STRING_STATIC("function"), 1},
    [REL_FLAG]                 = {CORE_STRING_STATIC("flag"), 2},
    [REL_TRAP_KIND]            = {CORE_STRING_STATIC("trap_kind"), 2},
    [REL_ACCESS]               = {CORE_STRING_STATIC("access"), 3},
    [REL_PLACE_KIND]           = {CORE_STRING_STATIC("place_kind"), 2},
    [REL_PARAM_PLACE]          = {CORE_STRING_STATIC("param_place"), 2},
    [REL_CALL]                 = {CORE_STRING_STATIC("call"), 2},
    [REL_CALL_TARGET]          = {CORE_STRING_STATIC("call_target"), 2},
    [REL_CALL_UNKNOWN]         = {CORE_STRING_STATIC("call_unknown"), 1},
    [REL_OUT_ARGUMENT]         = {CORE_STRING_STATIC("out_argument"), 1},
    [REL_DISCARDED]            = {CORE_STRING_STATIC("discarded"), 1},
    [REL_ADDRESSABLE_ARGUMENT] = {CORE_STRING_STATIC("addressable_argument"),
                                  2},
    [REL_LOAD]                 = {CORE_STRING_STATIC("load"), 3},
    [REL_STORE]                = {CORE_STRING_STATIC("store"), 3},
    [REL_RESULT_EFFECT]        = {CORE_STRING_STATIC("result_effect"), 1},
    [REL_SHARED_PLACE]         = {CORE_STRING_STATIC("shared_place"), 1},
    [REL_VISIBLE_WRITE]        = {CORE_STRING_STATIC("visible_write"), 1},
    [REL_SIDE_EFFECT]          = {CORE_STRING_STATIC("side_effect"), 1},
    [REL_EFFECTFUL_CALL]       = {CORE_STRING_STATIC("effectful_call"), 1},
    [REL_IGNORED_RESULT]       = {CORE_STRING_STATIC("ignored_result"), 1},
};

static const core_string_t flag_names[] = {
    CORE_STRING_STATIC("may_fail"),
    CORE_STRING_STATIC("may_trap"),
    CORE_STRING_STATIC("may_allocate"),
    CORE_STRING_STATIC("may_block"),
    CORE_STRING_STATIC("diverges"),
    CORE_STRING_STATIC("calls_unknown"),
    CORE_STRING_STATIC("may_io"),
    CORE_STRING_STATIC("no_return"),
    CORE_STRING_STATIC("may_collect"),
    CORE_STRING_STATIC("allocates_direct"),
    CORE_STRING_STATIC("unbounded_recursion"),
    CORE_STRING_STATIC("calls_finalizers"),
    CORE_STRING_STATIC("starts_threads"),
};

static const core_string_t trap_names[] = {
    CORE_STRING_STATIC("explicit"),
    CORE_STRING_STATIC("bounds"),
    CORE_STRING_STATIC("unwrap"),
    CORE_STRING_STATIC("arithmetic"),
};

static const core_string_t access_names[] = {
    CORE_STRING_STATIC("read"),
    CORE_STRING_STATIC("write"),
    CORE_STRING_STATIC("must_success"),
    CORE_STRING_STATIC("must_failure"),
    CORE_STRING_STATIC("reads_entry"),
};

static const core_string_t place_names[] = {
    [IR_SPLACE_PARAM]   = CORE_STRING_STATIC("param"),
    [IR_SPLACE_GLOBAL]  = CORE_STRING_STATIC("global"),
    [IR_SPLACE_THREAD]  = CORE_STRING_STATIC("thread"),
    [IR_SPLACE_HEAP]    = CORE_STRING_STATIC("heap"),
    [IR_SPLACE_UNKNOWN] = CORE_STRING_STATIC("unknown"),
};

typedef struct {
    relation_t    relation;
    bool          negated;
    uint8_t       arity;
    core_string_t arguments[3];
} atom_t;

typedef struct {
    atom_t  head;
    uint8_t body_count;
    atom_t  body[5];
} rule_t;

#define V(name) CORE_STRING_STATIC(name)
#define ATOM1(r, a) {r, false, 1, {V(a)}}
#define ATOM2(r, a, b) {r, false, 2, {V(a), V(b)}}
#define ATOM3(r, a, b, c) {r, false, 3, {V(a), V(b), V(c)}}
#define NOT1(r, a) {r, true, 1, {V(a)}}

static const rule_t rules[] = {
    {ATOM1(REL_SHARED_PLACE, "P"), 1,
     {ATOM2(REL_PLACE_KIND, "P", "global")}},
    {ATOM1(REL_SHARED_PLACE, "P"), 1,
     {ATOM2(REL_PLACE_KIND, "P", "thread")}},
    {ATOM1(REL_SHARED_PLACE, "P"), 1,
     {ATOM2(REL_PLACE_KIND, "P", "unknown")}},
    {ATOM1(REL_VISIBLE_WRITE, "S"), 2,
     {ATOM2(REL_CALL_TARGET, "S", "G"),
      ATOM2(REL_FLAG, "G", "calls_unknown")}},
    {ATOM1(REL_VISIBLE_WRITE, "S"), 3,
     {ATOM2(REL_CALL_TARGET, "S", "G"), ATOM3(REL_ACCESS, "G", "P", "write"),
      ATOM1(REL_SHARED_PLACE, "P")}},
    {ATOM1(REL_VISIBLE_WRITE, "S"), 4,
     {ATOM2(REL_CALL_TARGET, "S", "G"), ATOM3(REL_ACCESS, "G", "P", "write"),
      ATOM2(REL_PARAM_PLACE, "P", "I"),
      ATOM2(REL_ADDRESSABLE_ARGUMENT, "S", "I")}},
    {ATOM1(REL_SIDE_EFFECT, "S"), 1, {ATOM1(REL_OUT_ARGUMENT, "S")}},
    {ATOM1(REL_SIDE_EFFECT, "S"), 1, {ATOM1(REL_VISIBLE_WRITE, "S")}},
    {ATOM1(REL_EFFECTFUL_CALL, "S"), 3,
     {ATOM2(REL_CALL_TARGET, "S", "G"), ATOM2(REL_FLAG, "G", "K"),
      ATOM1(REL_RESULT_EFFECT, "K")}},
    {ATOM1(REL_IGNORED_RESULT, "S"), 4,
     {ATOM1(REL_DISCARDED, "S"), ATOM2(REL_CALL_TARGET, "S", "G"),
      NOT1(REL_VISIBLE_WRITE, "S"), NOT1(REL_EFFECTFUL_CALL, "S")}},
};

static const core_string_t result_effects[] = {
    CORE_STRING_STATIC("may_fail"),
    CORE_STRING_STATIC("may_block"),
    CORE_STRING_STATIC("diverges"),
    CORE_STRING_STATIC("calls_unknown"),
    CORE_STRING_STATIC("may_io"),
    CORE_STRING_STATIC("calls_finalizers"),
    CORE_STRING_STATIC("starts_threads"),
};

struct ir_summary_facts_t {
    core_datalog_engine_t          *engine;
    ir_summaries_t                 *summaries;
    core_datalog_relation_id_t      ids[REL_COUNT];
    core_list_t(facts_func_ptr_t)   functions;
    core_dict_t                     function_ids;
    core_dict_t                     site_symbols;
    core_list_t(ir_splace_t)        places;
    core_dict_t                     place_ids;
    core_dict_t                     param_place_ids;
    int64_t                         unknown_place;
    ir_summary_t                   *unknown_callee;
    int64_t                         next_site;
};

static core_datalog_symbol_t
constant(ir_summary_facts_t *facts, core_string_t name)
{
    return core_datalog_constant(facts->engine, name);
}

static core_datalog_symbol_t
integer(ir_summary_facts_t *facts, int64_t value)
{
    return core_datalog_integer(facts->engine, value);
}

static void
fact(ir_summary_facts_t *facts, relation_t relation,
     core_datalog_symbol_t a, core_datalog_symbol_t b, core_datalog_symbol_t c)
{
    core_datalog_symbol_t tuple[3] = {a, b, c};
    core_datalog_add_fact(facts->engine, facts->ids[relation],
                          relations[relation].arity, tuple);
}

static core_datalog_symbol_t
place_symbol(ir_summary_facts_t *facts, const ir_splace_t *place)
{
    core_dict_t *index = nullptr;
    const void  *key   = nullptr;
    switch (place->kind) {
    case IR_SPLACE_PARAM:
        index = &facts->param_place_ids;
        key   = (const void *)(uintptr_t)(place->param + 1);
        break;
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        index = &facts->place_ids;
        key   = place->global;
        break;
    case IR_SPLACE_HEAP:
        index = &facts->place_ids;
        key   = place->type ? (const void *)th_resolve(place->type)
                            : (const void *)facts;
        break;
    case IR_SPLACE_UNKNOWN:
        break;
    }
    if (!index || !key) {
        if (facts->unknown_place < 0) {
            facts->unknown_place = (int64_t)facts->places.len;
            core_list_push(facts->places,
                           ((ir_splace_t){.kind = IR_SPLACE_UNKNOWN}));
            fact(facts, REL_PLACE_KIND, integer(facts, facts->unknown_place),
                 constant(facts, place_names[IR_SPLACE_UNKNOWN]), 0);
        }
        return integer(facts, facts->unknown_place);
    }
    bool  found = false;
    void *id    = core_dict_get(index, key, &found);
    if (found) return integer(facts, (int64_t)(uintptr_t)id);
    int64_t next = (int64_t)facts->places.len;
    core_list_push(facts->places, *place);
    core_dict_put(index, key, (uintptr_t)next);
    core_datalog_symbol_t symbol = integer(facts, next);
    fact(facts, REL_PLACE_KIND, symbol,
         constant(facts, place_names[place->kind]), 0);
    if (place->kind == IR_SPLACE_PARAM)
        fact(facts, REL_PARAM_PLACE, symbol, integer(facts, place->param), 0);
    return symbol;
}

static void add_summary_facts(ir_summary_facts_t *facts,
                              core_datalog_symbol_t symbol,
                              const ir_summary_t *summary);

static core_datalog_symbol_t
function_symbol(ir_summary_facts_t *facts, const ir_func_t *function,
                const ir_summary_t *summary)
{
    const void *key   = function ? (const void *)function
                                 : (const void *)summary;
    bool        found = false;
    void       *id    = core_dict_get(&facts->function_ids, key, &found);
    if (found) return integer(facts, (int64_t)(uintptr_t)id);
    int64_t next = (int64_t)facts->functions.len;
    core_list_push(facts->functions, function);
    core_dict_put(&facts->function_ids, key, (uintptr_t)next);
    core_datalog_symbol_t symbol = integer(facts, next);
    fact(facts, REL_FUNCTION, symbol, 0, 0);
    if (!summary) return symbol;
    add_summary_facts(facts, symbol, summary);
    const uint32_t entries[] = {IR_SUMMARY_CALLS_FINALIZERS,
                                IR_SUMMARY_STARTS_THREADS};
    for (size_t e = 0; e < sizeof(entries) / sizeof(*entries); e++) {
        if (!(ir_summary_flags(summary) & entries[e])) continue;
        const ir_summary_t *entry = ir_summary_entries(facts->summaries,
                                                       entries[e]);
        add_summary_facts(facts, symbol, entry ? entry
                                               : facts->unknown_callee);
    }
    return symbol;
}

static void
add_summary_facts(ir_summary_facts_t *facts, core_datalog_symbol_t symbol,
                  const ir_summary_t *summary)
{
    uint32_t flags = ir_summary_flags(summary);
    for (uint32_t bit = 0; bit < sizeof(flag_names) / sizeof(*flag_names);
         bit++)
        if (flags & (1u << bit))
            fact(facts, REL_FLAG, symbol, constant(facts, flag_names[bit]), 0);
    uint8_t traps = ir_summary_trap_kinds(summary);
    for (uint32_t bit = 0; bit < sizeof(trap_names) / sizeof(*trap_names);
         bit++)
        if (traps & (1u << bit))
            fact(facts, REL_TRAP_KIND, symbol,
                 constant(facts, trap_names[bit]), 0);
    for (uint32_t a = 0; a < ir_summary_access_count(summary); a++) {
        const ir_saccess_t *access = ir_summary_access_at(summary, a);
        core_datalog_symbol_t place = place_symbol(facts, &access->place);
        for (uint32_t bit = 0;
             bit < sizeof(access_names) / sizeof(*access_names); bit++)
            if (access->flags & (1u << bit))
                fact(facts, REL_ACCESS, symbol, place,
                     constant(facts, access_names[bit]));
    }
}

static core_datalog_symbol_t
site_symbol(ir_summary_facts_t *facts, const ir_instr_t *in)
{
    bool  found = false;
    void *symbol = core_dict_get(&facts->site_symbols, in, &found);
    if (found) return (core_datalog_symbol_t)(uintptr_t)symbol;
    core_datalog_symbol_t next = integer(facts, facts->next_site++);
    core_dict_put(&facts->site_symbols, in, (uintptr_t)next);
    return next;
}

typedef struct {
    ir_summary_facts_t   *facts;
    const ir_instr_t     *call;
    core_datalog_symbol_t site;
} callee_visit_t;

static void
add_callee(void *context, const ir_func_t *target, const ir_summary_t *summary)
{
    callee_visit_t     *visit = context;
    ir_summary_facts_t *facts = visit->facts;
    fact(facts, REL_CALL_TARGET, visit->site,
         function_symbol(facts, target, summary), 0);
    for (uint32_t a = 0; summary && a < ir_summary_access_count(summary);
         a++) {
        const ir_saccess_t *access = ir_summary_access_at(summary, a);
        if (access->place.kind == IR_SPLACE_PARAM
            && access->place.param + 1 >= visit->call->n_operands)
            fact(facts, REL_ADDRESSABLE_ARGUMENT, visit->site,
                 integer(facts, access->place.param), 0);
    }
}

static bool
discarded(const ir_instr_t *in)
{
    const ir_annot_t *marked = ir_annot_get(&((ir_instr_t *)in)->annots,
        CORE_STRING_STATIC("ir.result_discarded"));
    return marked && marked->kind == IR_ANNOT_BOOL && marked->b;
}

static void
add_call(ir_summary_facts_t *facts, ir_func_t *function,
         core_datalog_symbol_t caller, ir_instr_t *in)
{
    core_datalog_symbol_t site = site_symbol(facts, in);
    fact(facts, REL_CALL, caller, site, 0);
    callee_visit_t visit = {facts, in, site};
    const ir_summary_t *callbacks = in->op == IR_OP_CALLBACK_INVOKE
        ? ir_summary_entries(facts->summaries, IR_SUMMARY_CALLS_CALLBACKS)
        : nullptr;
    if (!ir_summary_call_callees(facts->summaries, in, add_callee, &visit)) {
        if (callbacks) add_callee(&visit, nullptr, callbacks);
        else {
            fact(facts, REL_CALL_UNKNOWN, site, 0, 0);
            add_callee(&visit, nullptr, facts->unknown_callee);
        }
    }
    if (discarded(in)) fact(facts, REL_DISCARDED, site, 0, 0);
    for (uint32_t o = 1; o < in->n_operands; o++) {
        if (in->operands[o].kind == IR_OPD_OUT)
            fact(facts, REL_OUT_ARGUMENT, site, 0, 0);
        if (ir_summary_argument_addressable(function,
                                            ir_operand_value(&in->operands[o])))
            fact(facts, REL_ADDRESSABLE_ARGUMENT, site, integer(facts, o - 1),
                 0);
    }
}

static void
add_accesses(ir_summary_facts_t *facts, ir_func_t *function,
             core_datalog_symbol_t caller)
{
    ir_places_t *places = ir_func_places(function);
    for (uint32_t a = 0; places && a < ir_places_access_count(places); a++) {
        const ir_access_t *access = ir_places_access_at(places, a);
        const uint32_t    *ids    = ir_places_access_places(places, access);
        for (uint32_t p = 0; p < access->count; p++) {
            ir_splace_t place;
            if (!ir_summary_place_of(function, ir_place_at(places, ids[p]),
                                     &place)) continue;
            fact(facts, access->kind == IR_ACCESS_LOAD ? REL_LOAD : REL_STORE,
                 caller, site_symbol(facts, access->instr),
                 place_symbol(facts, &place));
        }
    }
}

static void
add_rules(ir_summary_facts_t *facts)
{
    for (size_t r = 0; r < sizeof(rules) / sizeof(*rules); r++) {
        core_datalog_rule_builder_t *builder = core_datalog_rule_builder_new();
        for (int b = -1; b < rules[r].body_count; b++) {
            const atom_t *atom = b < 0 ? &rules[r].head : &rules[r].body[b];
            core_datalog_symbol_t arguments[3];
            for (uint8_t i = 0; i < atom->arity; i++) {
                core_string_t name = atom->arguments[i];
                bool variable = name.data[0] >= 'A' && name.data[0] <= 'Z';
                arguments[i] = variable
                    ? core_datalog_variable(facts->engine, name)
                    : constant(facts, name);
            }
            if (b < 0)
                core_datalog_rule_builder_head(builder,
                    facts->ids[atom->relation], atom->arity, arguments);
            else
                core_datalog_rule_builder_add_literal(builder,
                    facts->ids[atom->relation], atom->arity, arguments,
                    atom->negated);
        }
        core_datalog_add_rule(facts->engine,
                              core_datalog_rule_builder_finish(builder));
    }
}

ir_summary_facts_t *
ir_summary_facts_build(ir_summaries_t *summaries, ir_func_t *const *functions,
                       size_t count, bool provenance)
{
    ir_summary_facts_t *facts = core_alloc(ir_summary_facts_t);
    facts->engine        = core_datalog_engine_new();
    facts->summaries     = summaries;
    facts->functions     = core_list_new(facts_func_ptr_t);
    facts->places        = core_list_new(ir_splace_t);
    facts->unknown_place = -1;
    facts->unknown_callee = ir_summary_conservative(summaries, nullptr);
    core_dict_init(&facts->function_ids, core_hash_word, nullptr);
    core_dict_init(&facts->site_symbols, core_hash_word, nullptr);
    core_dict_init(&facts->place_ids, core_hash_word, nullptr);
    core_dict_init(&facts->param_place_ids, core_hash_word, nullptr);
    core_datalog_set_provenance(facts->engine, provenance);
    for (int r = 0; r < REL_COUNT; r++)
        facts->ids[r] = core_datalog_relation(facts->engine,
            relations[r].name, relations[r].arity);
    for (size_t e = 0; e < sizeof(result_effects) / sizeof(*result_effects);
         e++)
        fact(facts, REL_RESULT_EFFECT, constant(facts, result_effects[e]), 0,
             0);
    for (size_t f = 0; f < count; f++) {
        ir_func_t *function = functions[f];
        if (!function->n_blocks) continue;
        core_datalog_symbol_t caller = function_symbol(facts, function,
            ir_summary_of(summaries, function));
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                if (in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                    add_call(facts, function, caller, in);
            }
        size_t scope = ir_analysis_scope_begin();
        add_accesses(facts, function, caller);
        ir_analysis_scope_end(scope);
    }
    add_rules(facts);
    core_datalog_run(facts->engine);
    return facts;
}

void
ir_summary_facts_free(ir_summary_facts_t *facts)
{
    if (!facts) return;
    core_datalog_engine_free(facts->engine);
    core_list_free(facts->functions);
    core_list_free(facts->places);
    core_dict_free(&facts->function_ids);
    core_dict_free(&facts->site_symbols);
    core_dict_free(&facts->place_ids);
    core_dict_free(&facts->param_place_ids);
    core_free(facts);
}

core_datalog_engine_t *
ir_summary_facts_engine(ir_summary_facts_t *facts)
{
    return facts ? facts->engine : nullptr;
}

static bool
site_holds(ir_summary_facts_t *facts, relation_t relation,
           const ir_instr_t *call)
{
    bool  found  = false;
    void *symbol = core_dict_get(&facts->site_symbols, call, &found);
    if (!found) return false;
    core_datalog_symbol_t site = (core_datalog_symbol_t)(uintptr_t)symbol;
    return core_datalog_contains(facts->engine, facts->ids[relation], &site);
}

bool
ir_summary_facts_side_effect(ir_summary_facts_t *facts, const ir_instr_t *call)
{
    return site_holds(facts, REL_SIDE_EFFECT, call);
}

bool
ir_summary_facts_ignored_result(ir_summary_facts_t *facts,
                                const ir_instr_t *call)
{
    return site_holds(facts, REL_IGNORED_RESULT, call);
}

typedef struct {
    ir_summary_facts_t *facts;
    ir_effect_reason_t *reason;
} explain_t;

static int64_t
symbol_index(ir_summary_facts_t *facts, core_datalog_symbol_t symbol)
{
    core_datalog_int_result_t value = core_datalog_symbol_integer(
        facts->engine, symbol);
    return value.is_ok ? value.result.value : -1;
}

static bool
record_reason(core_datalog_relation_id_t relation,
              const core_datalog_symbol_t *tuple, int32_t arity, int32_t rule,
              int32_t depth, void *context)
{
    (void)arity;
    (void)rule;
    (void)depth;
    explain_t          *explain = context;
    ir_summary_facts_t *facts   = explain->facts;
    ir_effect_reason_t *reason  = explain->reason;
    if (reason->kind == IR_EFFECT_REASON_UNKNOWN_CALLEE) return false;
    if (relation == facts->ids[REL_OUT_ARGUMENT])
        reason->kind = IR_EFFECT_REASON_OUT_ARGUMENT;
    else if (relation == facts->ids[REL_CALL_TARGET]) {
        int64_t index = symbol_index(facts, tuple[1]);
        if (index >= 0 && (size_t)index < facts->functions.len)
            reason->callee = facts->functions.data[index];
        if (!reason->callee) reason->kind = IR_EFFECT_REASON_UNKNOWN_CALLEE;
    } else if (relation == facts->ids[REL_FLAG])
        reason->kind = IR_EFFECT_REASON_CALLEE_CALLS_UNKNOWN;
    else if (relation == facts->ids[REL_ACCESS]) {
        int64_t index = symbol_index(facts, tuple[1]);
        reason->kind = IR_EFFECT_REASON_CALLEE_WRITES;
        if (index >= 0 && (size_t)index < facts->places.len)
            reason->place = facts->places.data[index];
    }
    return true;
}

bool
ir_summary_facts_explain_side_effect(ir_summary_facts_t *facts,
                                     const ir_instr_t *call,
                                     ir_effect_reason_t *reason)
{
    *reason = (ir_effect_reason_t){.kind = IR_EFFECT_REASON_NONE};
    bool  found  = false;
    void *symbol = core_dict_get(&facts->site_symbols, call, &found);
    if (!found) return false;
    core_datalog_symbol_t site = (core_datalog_symbol_t)(uintptr_t)symbol;
    explain_t explain = {facts, reason};
    return core_datalog_explain(facts->engine, facts->ids[REL_SIDE_EFFECT],
                                &site, record_reason, &explain)
        && reason->kind != IR_EFFECT_REASON_NONE;
}
