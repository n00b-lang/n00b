#ifndef N00BB_COMPILER_EFFECTS_H
#define N00BB_COMPILER_EFFECTS_H

#include "ir/summary.h"
#include "lib/buffer.h"
#include "specialize.h"

typedef struct {
    core_string_t name;
    uint64_t      bits;
    bool          group;
} n00bb_effect_name_t;

enum {
    N00BB_ROLE_READS  = 1u << 0,
    N00BB_ROLE_WRITES = 1u << 1,
    N00BB_ROLE_LENDS  = 1u << 2,
};

typedef enum {
    N00BB_QUERY_NONE,
    N00BB_QUERY_MAY_EFFECT,
    N00BB_QUERY_FORBIDS_EFFECT,
    N00BB_QUERY_HAS_EFFECT,
    N00BB_QUERY_MAY_READ,
    N00BB_QUERY_MAY_WRITE,
    N00BB_QUERY_MAY_LEND,
    N00BB_QUERY_MAY_CARRY,
} n00bb_query_t;

size_t n00bb_effect_name_count(void);
const n00bb_effect_name_t *n00bb_effect_name_at(size_t index);
const n00bb_effect_name_t *n00bb_effect_named(core_string_t name);
uint64_t n00bb_named_effect_bits(void);
uint8_t n00bb_parameter_role(const ir_func_t *function,
                             const ir_summary_t *summary, uint32_t param);
typedef struct {
    n00bb_query_t query;
    uint32_t      param;
    core_string_t effect;
} n00bb_bound_t;

typedef struct {
    ir_func_t                      *function;
    core_string_t                   native;
    struct core_translation_unit_t *unit;
} n00bb_effect_clause_t;
core_list_decl(n00bb_effect_clause_t);

typedef enum {
    N00BB_EFFECT_SCOPE_THREAD,
    N00BB_EFFECT_SCOPE_PACKAGE,
    N00BB_EFFECT_SCOPE_CALLER,
} n00bb_effect_scope_t;

typedef struct {
    core_string_t                      qualified;
    uint64_t                           bits;
    core_list_t(n00bb_effect_clause_t) sources;
    core_list_t(n00bb_effect_clause_t) handlers;
    n00bb_effect_scope_t               scope;
    struct n00bb_link_analysis_t      *analysis;
} n00bb_user_effect_t;

bool n00bb_effect_scope_named(core_string_t word, n00bb_effect_scope_t *out);
bool n00bb_effect_crosses(const n00bb_user_effect_t *user,
                          const ir_func_t *caller, const ir_func_t *callee);

n00bb_query_t n00bb_query_named(core_string_t word);
core_string_t n00bb_query_word(n00bb_query_t query);
bool n00bb_query_about_effect(n00bb_query_t query);
int64_t n00bb_parameter_named(const ir_func_t *function, core_string_t name);
bool n00bb_must_effect(ir_summaries_t *summaries, ir_func_t *function,
                       uint64_t bits, const n00bb_user_effect_t *user);
const ir_instr_t *n00bb_must_skip(ir_summaries_t *summaries,
                                  ir_func_t *function, uint64_t bits,
                                  const n00bb_user_effect_t *user);
bool n00bb_query_answer(ir_summaries_t *summaries, ir_func_t *subject,
                        n00bb_query_t query, core_string_t name,
                        const n00bb_user_effect_t *user);
bool n00bb_clause_matches(const n00bb_effect_clause_t *clause,
                          ir_func_t *body);
bool n00bb_function_listed(
    const core_list_t(n00bb_effect_clause_t) *clauses, ir_func_t *function);
bool n00bb_bound_parse(core_string_t tag, n00bb_bound_t *out);
void n00bb_bound_tag(core_buffer_t *out, n00bb_query_t query, uint32_t param,
                     core_string_t effect);
uint64_t n00bb_bound_bits(const ir_func_t *function, n00bb_query_t query);
bool n00bb_bound_names(const ir_func_t *function, n00bb_query_t query,
                       core_string_t effect);

static const core_string_t n00bb_callback_bound_key =
    CORE_STRING_STATIC("n00bb.callback_bound");

static inline ir_annot_t *
n00bb_callback_bound(const ir_func_t *function)
{
    return function ? ir_annot_get(&((ir_func_t *)function)->annots,
                                   n00bb_callback_bound_key)
                    : nullptr;
}

static inline bool
n00bb_is_callback_bound(const ir_func_t *function)
{
    return function && !function->n_blocks && n00bb_callback_bound(function);
}

#endif
