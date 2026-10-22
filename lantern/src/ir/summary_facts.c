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
    REL_FLAG,
    REL_ACCESS,
    REL_PLACE_KIND,
    REL_PARAM_PLACE,
    REL_CALL_TARGET,
    REL_CALL_UNKNOWN,
    REL_OUT_ARGUMENT,
    REL_DISCARDED,
    REL_ADDRESSABLE_ARGUMENT,
    REL_RESULT_EFFECT,
    REL_ASSERT_EFFECT,
    REL_ESCAPE,
    REL_RETURN_ALIAS,
    REL_DEPENDENCE,
    REL_MEMBER,
    REL_FIELD,
    REL_SHARED_PLACE,
    REL_VISIBLE_WRITE,
    REL_SIDE_EFFECT,
    REL_EFFECTFUL_CALL,
    REL_IGNORED_RESULT,
    REL_RESULT_ALIAS,
    REL_RESULT_INPUT,
    REL_DIVERGING_TARGET,
    REL_CALL_RETURNS,
    REL_CALL_DIVERGES,
    REL_CALL_READS_ENTRY,
    REL_CALL_READS_UNSEEN,
    REL_FIELD_READ,
    REL_FIELD_WRITTEN,
    REL_RECORD_READ,
    REL_COUNT,
} relation_t;

static const struct {
    core_string_t name;
    int32_t       arity;
} relations[REL_COUNT] = {
    [REL_FLAG]                 = {CORE_STRING_STATIC("flag"), 2},
    [REL_ACCESS]               = {CORE_STRING_STATIC("access"), 3},
    [REL_PLACE_KIND]           = {CORE_STRING_STATIC("place_kind"), 2},
    [REL_PARAM_PLACE]          = {CORE_STRING_STATIC("param_place"), 2},
    [REL_CALL_TARGET]          = {CORE_STRING_STATIC("call_target"), 2},
    [REL_CALL_UNKNOWN]         = {CORE_STRING_STATIC("call_unknown"), 1},
    [REL_OUT_ARGUMENT]         = {CORE_STRING_STATIC("out_argument"), 1},
    [REL_DISCARDED]            = {CORE_STRING_STATIC("discarded"), 1},
    [REL_ADDRESSABLE_ARGUMENT] = {CORE_STRING_STATIC("addressable_argument"),
                                  2},
    [REL_RESULT_EFFECT]        = {CORE_STRING_STATIC("result_effect"), 1},
    [REL_ASSERT_EFFECT]        = {CORE_STRING_STATIC("assert_effect"), 1},
    [REL_ESCAPE]               = {CORE_STRING_STATIC("escape"), 2},
    [REL_RETURN_ALIAS]         = {CORE_STRING_STATIC("return_alias"), 2},
    [REL_DEPENDENCE]           = {CORE_STRING_STATIC("dependence"), 2},
    [REL_MEMBER]               = {CORE_STRING_STATIC("member"), 1},
    [REL_FIELD]                = {CORE_STRING_STATIC("field"), 4},
    [REL_SHARED_PLACE]         = {CORE_STRING_STATIC("shared_place"), 1},
    [REL_VISIBLE_WRITE]        = {CORE_STRING_STATIC("visible_write"), 1},
    [REL_SIDE_EFFECT]          = {CORE_STRING_STATIC("side_effect"), 1},
    [REL_EFFECTFUL_CALL]       = {CORE_STRING_STATIC("effectful_call"), 1},
    [REL_IGNORED_RESULT]       = {CORE_STRING_STATIC("ignored_result"), 1},
    [REL_RESULT_ALIAS]         = {CORE_STRING_STATIC("result_alias"), 2},
    [REL_RESULT_INPUT]         = {CORE_STRING_STATIC("result_input"), 2},
    [REL_DIVERGING_TARGET]     = {CORE_STRING_STATIC("diverging_target"), 2},
    [REL_CALL_RETURNS]         = {CORE_STRING_STATIC("call_returns"), 1},
    [REL_CALL_DIVERGES]        = {CORE_STRING_STATIC("call_diverges"), 1},
    [REL_CALL_READS_ENTRY]     = {CORE_STRING_STATIC("call_reads_entry"), 2},
    [REL_CALL_READS_UNSEEN]    = {CORE_STRING_STATIC("call_reads_unseen"), 1},
    [REL_FIELD_READ]           = {CORE_STRING_STATIC("field_read"), 2},
    [REL_FIELD_WRITTEN]        = {CORE_STRING_STATIC("field_written"), 2},
    [REL_RECORD_READ]          = {CORE_STRING_STATIC("record_read"), 1},
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

static const core_string_t access_names[] = {
    CORE_STRING_STATIC("read"),
    CORE_STRING_STATIC("write"),
    CORE_STRING_STATIC("must_success"),
    CORE_STRING_STATIC("must_failure"),
    CORE_STRING_STATIC("reads_entry"),
};

static const core_string_t field_names[] = {
    CORE_STRING_STATIC("read"),
    CORE_STRING_STATIC("write"),
    CORE_STRING_STATIC("whole"),
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
    core_string_t arguments[4];
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
#define ATOM4(r, a, b, c, d) {r, false, 4, {V(a), V(b), V(c), V(d)}}
#define NOT1(r, a) {r, true, 1, {V(a)}}
#define NOT2(r, a, b) {r, true, 2, {V(a), V(b)}}

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
    {ATOM1(REL_VISIBLE_WRITE, "S"), 3,
     {ATOM2(REL_CALL_TARGET, "S", "G"), ATOM2(REL_ESCAPE, "G", "I"),
      ATOM2(REL_ADDRESSABLE_ARGUMENT, "S", "I")}},
    {ATOM1(REL_SIDE_EFFECT, "S"), 1, {ATOM1(REL_OUT_ARGUMENT, "S")}},
    {ATOM1(REL_SIDE_EFFECT, "S"), 1, {ATOM1(REL_VISIBLE_WRITE, "S")}},
    {ATOM1(REL_SIDE_EFFECT, "S"), 3,
     {ATOM2(REL_CALL_TARGET, "S", "G"), ATOM2(REL_FLAG, "G", "K"),
      ATOM1(REL_ASSERT_EFFECT, "K")}},
    {ATOM1(REL_EFFECTFUL_CALL, "S"), 3,
     {ATOM2(REL_CALL_TARGET, "S", "G"), ATOM2(REL_FLAG, "G", "K"),
      ATOM1(REL_RESULT_EFFECT, "K")}},
    {ATOM1(REL_IGNORED_RESULT, "S"), 4,
     {ATOM1(REL_DISCARDED, "S"), ATOM2(REL_CALL_TARGET, "S", "G"),
      NOT1(REL_VISIBLE_WRITE, "S"), NOT1(REL_EFFECTFUL_CALL, "S")}},
    {ATOM2(REL_RESULT_ALIAS, "S", "I"), 3,
     {ATOM1(REL_IGNORED_RESULT, "S"), ATOM2(REL_CALL_TARGET, "S", "G"),
      ATOM2(REL_RETURN_ALIAS, "G", "I")}},
    {ATOM2(REL_RESULT_INPUT, "S", "I"), 3,
     {ATOM1(REL_IGNORED_RESULT, "S"), ATOM2(REL_CALL_TARGET, "S", "G"),
      ATOM2(REL_DEPENDENCE, "G", "I")}},
    {ATOM2(REL_DIVERGING_TARGET, "S", "G"), 2,
     {ATOM2(REL_CALL_TARGET, "S", "G"), ATOM2(REL_FLAG, "G", "diverges")}},
    {ATOM1(REL_CALL_RETURNS, "S"), 2,
     {ATOM2(REL_CALL_TARGET, "S", "G"), NOT2(REL_FLAG, "G", "diverges")}},
    {ATOM1(REL_CALL_RETURNS, "S"), 1, {ATOM1(REL_CALL_UNKNOWN, "S")}},
    {ATOM1(REL_CALL_DIVERGES, "S"), 2,
     {ATOM2(REL_DIVERGING_TARGET, "S", "G"), NOT1(REL_CALL_RETURNS, "S")}},
    {ATOM2(REL_CALL_READS_ENTRY, "S", "P"), 2,
     {ATOM2(REL_CALL_TARGET, "S", "G"),
      ATOM3(REL_ACCESS, "G", "P", "reads_entry")}},
    {ATOM1(REL_CALL_READS_UNSEEN, "S"), 2,
     {ATOM2(REL_CALL_READS_ENTRY, "S", "P"),
      ATOM2(REL_PLACE_KIND, "P", "unknown")}},
    {ATOM2(REL_FIELD_READ, "R", "F"), 2,
     {ATOM1(REL_MEMBER, "G"), ATOM4(REL_FIELD, "G", "R", "F", "read")}},
    {ATOM2(REL_FIELD_WRITTEN, "R", "F"), 2,
     {ATOM1(REL_MEMBER, "G"), ATOM4(REL_FIELD, "G", "R", "F", "write")}},
    {ATOM1(REL_RECORD_READ, "R"), 2,
     {ATOM1(REL_MEMBER, "G"), ATOM4(REL_FIELD, "G", "R", "F", "whole")}},
};

static const core_string_t result_effects[] = {
    CORE_STRING_STATIC("may_fail"),
    CORE_STRING_STATIC("traps_explicit"),
    CORE_STRING_STATIC("may_block"),
    CORE_STRING_STATIC("diverges"),
    CORE_STRING_STATIC("calls_unknown"),
    CORE_STRING_STATIC("may_io"),
    CORE_STRING_STATIC("calls_finalizers"),
    CORE_STRING_STATIC("starts_threads"),
};

static const core_string_t assert_effects[] = {
    CORE_STRING_STATIC("may_io"),
    CORE_STRING_STATIC("may_block"),
    CORE_STRING_STATIC("diverges"),
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

static const void *
place_key(ir_summary_facts_t *facts, const ir_splace_t *place,
          core_dict_t **index)
{
    switch (place->kind) {
    case IR_SPLACE_PARAM:
        *index = &facts->param_place_ids;
        return (const void *)(uintptr_t)(place->param + 1);
    case IR_SPLACE_GLOBAL:
    case IR_SPLACE_THREAD:
        *index = &facts->place_ids;
        return place->global ? (const void *)ir_global_identity(place->global)
                             : nullptr;
    case IR_SPLACE_HEAP:
        *index = &facts->place_ids;
        return place->type ? (const void *)th_resolve(place->type)
                           : (const void *)facts;
    case IR_SPLACE_UNKNOWN:
        break;
    }
    *index = nullptr;
    return nullptr;
}

static core_datalog_symbol_t
place_symbol(ir_summary_facts_t *facts, const ir_splace_t *place)
{
    core_dict_t *index = nullptr;
    const void  *key   = place_key(facts, place, &index);
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

static void
add_summary_facts(ir_summary_facts_t *facts, core_datalog_symbol_t symbol,
                  const ir_summary_t *summary, uint32_t mask)
{
    uint32_t flags = ir_summary_flags(summary) & mask;
    for (uint32_t bit = 0; bit < sizeof(flag_names) / sizeof(*flag_names);
         bit++)
        if (flags & (1u << bit))
            fact(facts, REL_FLAG, symbol, constant(facts, flag_names[bit]), 0);
    if ((flags & IR_SUMMARY_MAY_TRAP)
        && (ir_summary_trap_kinds(summary) & IR_STRAP_EXPLICIT))
        fact(facts, REL_FLAG, symbol,
             constant(facts, CORE_STRING_STATIC("traps_explicit")), 0);
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

static void
add_result_facts(ir_summary_facts_t *facts, core_datalog_symbol_t symbol,
                 const ir_summary_t *summary)
{
    ir_sreturn_t returned = ir_summary_return(summary);
    if (returned.kind == IR_SRETURN_PARAM)
        fact(facts, REL_RETURN_ALIAS, symbol, integer(facts, returned.param),
             0);
    const uint32_t *dependences = ir_summary_dependences(summary);
    for (uint32_t o = 0; o < ir_summary_output_count(summary); o++) {
        const ir_soutput_t *output = ir_summary_output_at(summary, o);
        if (output->kind != IR_SOUTPUT_RESULT) continue;
        for (uint32_t d = 0; d < output->data_count; d++) {
            const ir_sinput_t *input = ir_summary_input_at(summary,
                dependences[output->data_first + d]);
            if (input && (input->kind == IR_SINPUT_VALUE
                          || input->kind == IR_SINPUT_REFERENT))
                fact(facts, REL_DEPENDENCE, symbol,
                     integer(facts, input->param), 0);
        }
    }
}

static core_string_t
record_name(th_type *record)
{
    core_string_t name = {0};
    th_type *type = record ? th_resolve(record) : nullptr;
    if (!type || th_kind_of(type) != TH_NAMED
        || !th_named_info(type, &name, nullptr, nullptr))
        return (core_string_t){0};
    return name;
}

static void
add_field_facts(ir_summary_facts_t *facts, core_datalog_symbol_t symbol,
                const ir_summary_t *summary)
{
    for (uint32_t i = 0; i < ir_summary_field_count(summary); i++) {
        const ir_sfield_t *field = ir_summary_field_at(summary, i);
        core_string_t record = record_name(field->record);
        if (!record.data) continue;
        for (uint32_t bit = 0;
             bit < sizeof(field_names) / sizeof(*field_names); bit++) {
            if (!(field->flags & (1u << bit))) continue;
            core_datalog_symbol_t tuple[4] = {
                symbol, constant(facts, record),
                field->name.data ? constant(facts, field->name)
                                 : constant(facts, record),
                constant(facts, field_names[bit]),
            };
            core_datalog_add_fact(facts->engine, facts->ids[REL_FIELD], 4,
                                  tuple);
        }
    }
}

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
    if (!summary) return symbol;
    add_summary_facts(facts, symbol, summary, UINT32_MAX);
    add_result_facts(facts, symbol, summary);
    const uint32_t entries[] = {IR_SUMMARY_CALLS_FINALIZERS,
                                IR_SUMMARY_STARTS_THREADS};
    for (size_t e = 0; e < sizeof(entries) / sizeof(*entries); e++) {
        if (!(ir_summary_flags(summary) & entries[e])) continue;
        const ir_summary_t *entry = ir_summary_entries(facts->summaries,
                                                       entries[e]);
        add_summary_facts(facts, symbol,
                          entry ? entry : facts->unknown_callee,
                          ~(uint32_t)(IR_SUMMARY_DIVERGES
                                      | IR_SUMMARY_NO_RETURN));
    }
    return symbol;
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
    core_datalog_symbol_t callee = function_symbol(facts, target, summary);
    fact(facts, REL_CALL_TARGET, visit->site, callee, 0);
    for (uint32_t a = 0; summary && a < ir_summary_access_count(summary);
         a++) {
        const ir_saccess_t *access = ir_summary_access_at(summary, a);
        if (access->place.kind == IR_SPLACE_PARAM
            && access->place.param + 1 >= visit->call->n_operands)
            fact(facts, REL_ADDRESSABLE_ARGUMENT, visit->site,
                 integer(facts, access->place.param), 0);
    }
    for (uint32_t o = 1; summary && o < visit->call->n_operands; o++)
        if (ir_summary_param_escape(summary, o - 1) & IR_SESCAPE_ADDRESS)
            fact(facts, REL_ESCAPE, callee, integer(facts, o - 1), 0);
}

static bool
discarded(const ir_instr_t *in)
{
    const ir_annot_t *marked = ir_annot_get(&((ir_instr_t *)in)->annots,
        CORE_STRING_STATIC("ir.result_discarded"));
    return marked && marked->kind == IR_ANNOT_BOOL && marked->b;
}

static void
add_call(ir_summary_facts_t *facts, ir_func_t *function, ir_instr_t *in)
{
    core_datalog_symbol_t site = site_symbol(facts, in);
    callee_visit_t visit = {facts, in, site};
    const ir_summary_t *callbacks = in->op == IR_OP_CALLBACK_INVOKE
        ? ir_summary_entries(facts->summaries, IR_SUMMARY_CALLS_CALLBACKS)
        : nullptr;
    if (!ir_summary_call_callees(facts->summaries, in, add_callee, &visit)) {
        fact(facts, REL_CALL_UNKNOWN, site, 0, 0);
        add_callee(&visit, nullptr, callbacks ? callbacks
                                              : facts->unknown_callee);
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
add_rules(ir_summary_facts_t *facts)
{
    for (size_t r = 0; r < sizeof(rules) / sizeof(*rules); r++) {
        core_datalog_rule_builder_t *builder = core_datalog_rule_builder_new();
        for (int b = -1; b < rules[r].body_count; b++) {
            const atom_t *atom = b < 0 ? &rules[r].head : &rules[r].body[b];
            core_datalog_symbol_t arguments[4];
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
    for (size_t e = 0; e < sizeof(assert_effects) / sizeof(*assert_effects);
         e++)
        fact(facts, REL_ASSERT_EFFECT, constant(facts, assert_effects[e]), 0,
             0);
    for (size_t f = 0; f < count; f++) {
        ir_func_t *function = functions[f];
        if (!function->n_blocks) continue;
        const ir_summary_t *summary = ir_summary_of(summaries, function);
        core_datalog_symbol_t member = function_symbol(facts, function,
                                                       summary);
        fact(facts, REL_MEMBER, member, 0, 0);
        if (summary) add_field_facts(facts, member, summary);
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                if (in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                    add_call(facts, function, in);
            }
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
site_of(ir_summary_facts_t *facts, const ir_instr_t *call,
        core_datalog_symbol_t *site)
{
    bool  found  = false;
    void *symbol = facts
        ? core_dict_get(&facts->site_symbols, call, &found) : nullptr;
    if (found) *site = (core_datalog_symbol_t)(uintptr_t)symbol;
    return found;
}

static bool
site_holds(ir_summary_facts_t *facts, relation_t relation,
           const ir_instr_t *call)
{
    core_datalog_symbol_t site;
    return site_of(facts, call, &site)
        && core_datalog_contains(facts->engine, facts->ids[relation], &site);
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

bool
ir_summary_facts_call_diverges(ir_summary_facts_t *facts,
                               const ir_instr_t *call)
{
    return site_holds(facts, REL_CALL_DIVERGES, call);
}

bool
ir_summary_facts_call_reads_entry(ir_summary_facts_t *facts,
                                  const ir_instr_t *call,
                                  const ir_splace_t *place)
{
    core_datalog_symbol_t site;
    if (!site_of(facts, call, &site)) return true;
    if (core_datalog_contains(facts->engine, facts->ids[REL_CALL_READS_UNSEEN],
                              &site)
        && ir_summary_place_unseen(facts->summaries, place))
        return true;
    core_dict_t *index = nullptr;
    const void  *key   = place_key(facts, place, &index);
    bool         found = false;
    void        *id    = index && key ? core_dict_get(index, key, &found)
                                      : nullptr;
    if (!found) return false;
    core_datalog_symbol_t tuple[2] = {site,
                                      integer(facts, (int64_t)(uintptr_t)id)};
    return core_datalog_contains(facts->engine,
                                 facts->ids[REL_CALL_READS_ENTRY], tuple);
}

uint8_t
ir_summary_facts_field_use(ir_summary_facts_t *facts, core_string_t record,
                           core_string_t field)
{
    if (!facts) return 0;
    core_datalog_symbol_t tuple[2] = {constant(facts, record),
                                      constant(facts, field)};
    uint8_t flags = 0;
    if (core_datalog_contains(facts->engine, facts->ids[REL_RECORD_READ],
                              tuple)
        || core_datalog_contains(facts->engine, facts->ids[REL_FIELD_READ],
                                 tuple))
        flags |= IR_SFIELD_READ;
    if (core_datalog_contains(facts->engine, facts->ids[REL_FIELD_WRITTEN],
                              tuple))
        flags |= IR_SFIELD_WRITE;
    return flags;
}

typedef struct {
    ir_summary_facts_t     *facts;
    ir_summary_explain_fn   visit;
    void                   *context;
} explain_t;

static int64_t
symbol_index(ir_summary_facts_t *facts, core_datalog_symbol_t symbol)
{
    core_datalog_int_result_t value = core_datalog_symbol_integer(
        facts->engine, symbol);
    return value.is_ok ? value.result.value : -1;
}

static const ir_func_t *
function_at(ir_summary_facts_t *facts, core_datalog_symbol_t symbol)
{
    int64_t index = symbol_index(facts, symbol);
    return index >= 0 && (size_t)index < facts->functions.len
        ? facts->functions.data[index] : nullptr;
}

static bool
record_step(core_datalog_relation_id_t relation,
            const core_datalog_symbol_t *tuple, int32_t arity, int32_t rule,
            int32_t depth, void *context)
{
    (void)arity;
    (void)rule;
    explain_t          *explain = context;
    ir_summary_facts_t *facts   = explain->facts;
    ir_summary_step_t   step    = {.depth = depth};
    if (relation == facts->ids[REL_OUT_ARGUMENT])
        step.kind = IR_SUMMARY_STEP_OUT_ARGUMENT;
    else if (relation == facts->ids[REL_DISCARDED])
        step.kind = IR_SUMMARY_STEP_DISCARDED;
    else if (relation == facts->ids[REL_CALL_TARGET]) {
        step.callee = function_at(facts, tuple[1]);
        step.kind   = step.callee ? IR_SUMMARY_STEP_CALLS
                                  : IR_SUMMARY_STEP_CALLS_UNSEEN;
    } else if (relation == facts->ids[REL_FLAG]) {
        step.callee = function_at(facts, tuple[0]);
        step.kind   = IR_SUMMARY_STEP_FLAG;
        step.name   = core_datalog_symbol_name(facts->engine, tuple[1]);
    } else if (relation == facts->ids[REL_ACCESS]) {
        int64_t index = symbol_index(facts, tuple[1]);
        step.callee = function_at(facts, tuple[0]);
        step.kind   = IR_SUMMARY_STEP_ACCESS;
        step.name   = core_datalog_symbol_name(facts->engine, tuple[2]);
        if (index >= 0 && (size_t)index < facts->places.len)
            step.place = facts->places.data[index];
    } else if (relation == facts->ids[REL_ESCAPE]) {
        step.callee = function_at(facts, tuple[0]);
        step.kind   = IR_SUMMARY_STEP_ESCAPE;
        step.param  = (uint32_t)symbol_index(facts, tuple[1]);
    } else if (relation == facts->ids[REL_ADDRESSABLE_ARGUMENT]) {
        step.kind  = IR_SUMMARY_STEP_ADDRESSABLE;
        step.param = (uint32_t)symbol_index(facts, tuple[1]);
    } else if (relation == facts->ids[REL_PLACE_KIND]) {
        int64_t index = symbol_index(facts, tuple[0]);
        step.kind = IR_SUMMARY_STEP_PLACE;
        if (index >= 0 && (size_t)index < facts->places.len)
            step.place = facts->places.data[index];
    } else if (relation == facts->ids[REL_RETURN_ALIAS]) {
        step.callee = function_at(facts, tuple[0]);
        step.kind   = IR_SUMMARY_STEP_RETURNS;
        step.param  = (uint32_t)symbol_index(facts, tuple[1]);
    } else if (relation == facts->ids[REL_DEPENDENCE]) {
        step.callee = function_at(facts, tuple[0]);
        step.kind   = IR_SUMMARY_STEP_DEPENDS;
        step.param  = (uint32_t)symbol_index(facts, tuple[1]);
    } else
        step.kind = IR_SUMMARY_STEP_RULE;
    step.relation = core_datalog_relation_name(facts->engine, relation);
    return explain->visit(explain->context, &step);
}

static bool
explain_site(ir_summary_facts_t *facts, relation_t relation,
             const ir_instr_t *call, ir_summary_explain_fn visit,
             void *context)
{
    core_datalog_symbol_t site;
    explain_t explain = {facts, visit, context};
    return site_of(facts, call, &site)
        && core_datalog_explain(facts->engine, facts->ids[relation], &site,
                                record_step, &explain);
}

bool
ir_summary_facts_explain_side_effect(ir_summary_facts_t *facts,
                                     const ir_instr_t *call,
                                     ir_summary_explain_fn visit,
                                     void *context)
{
    return explain_site(facts, REL_SIDE_EFFECT, call, visit, context);
}

bool
ir_summary_facts_explain_divergence(ir_summary_facts_t *facts,
                                    const ir_instr_t *call,
                                    ir_summary_explain_fn visit,
                                    void *context)
{
    return explain_site(facts, REL_CALL_DIVERGES, call, visit, context);
}

typedef struct {
    ir_summary_facts_t   *facts;
    core_datalog_symbol_t site;
    relation_t            relation;
    ir_summary_explain_fn visit;
    void                 *context;
    bool                  ok;
} result_visit_t;

static bool
explain_result_tuple(const core_datalog_symbol_t *tuple, int32_t arity,
                     void *context)
{
    (void)arity;
    result_visit_t *walk = context;
    if (tuple[0] != walk->site) return true;
    explain_t explain = {walk->facts, walk->visit, walk->context};
    walk->ok = core_datalog_explain(walk->facts->engine,
                                    walk->facts->ids[walk->relation], tuple,
                                    record_step, &explain);
    return walk->ok;
}

bool
ir_summary_facts_explain_ignored_result(ir_summary_facts_t *facts,
                                        const ir_instr_t *call,
                                        ir_summary_explain_fn visit,
                                        void *context)
{
    result_visit_t walk = {.facts = facts, .visit = visit,
                           .context = context, .ok = true};
    if (!site_of(facts, call, &walk.site)) return false;
    if (!explain_site(facts, REL_IGNORED_RESULT, call, visit, context))
        return false;
    const relation_t sources[] = {REL_RESULT_ALIAS, REL_RESULT_INPUT};
    for (size_t s = 0; walk.ok && s < sizeof(sources) / sizeof(*sources);
         s++) {
        walk.relation = sources[s];
        core_datalog_query(facts->engine, facts->ids[sources[s]],
                           explain_result_tuple, &walk);
    }
    return true;
}
