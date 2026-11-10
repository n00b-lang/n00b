#ifndef N00BB_COMPILER_TYPES_H
#define N00BB_COMPILER_TYPES_H

#include "compiler_link.h"
#include "compiler_tu.h"
#include "ir/abcd.h"
#include "ir/pdg.h"

typedef struct n00bb_dispatch_index n00bb_dispatch_index_t;

typedef enum : uint8_t {
    N00BB_TARGET_NONE,
    N00BB_TARGET_METHOD,
    N00BB_TARGET_ENUM,
    N00BB_TARGET_CALLBACK,
    N00BB_TARGET_FUNCTION,
    N00BB_TARGET_REFERENCE,
    N00BB_TARGET_TRAIT,
    N00BB_TARGET_FLAGSET,
    N00BB_TARGET_EQUAL_BY_CMP,
    N00BB_TARGET_CASE,
    N00BB_TARGET_REF_CREATE,
    N00BB_TARGET_REF_PLAIN,
    N00BB_TARGET_ASSIGN,
    N00BB_TARGET_PRIMITIVE,
} n00bb_target_kind_t;

typedef struct {
    n00bb_target_kind_t kind;
    th_type *signature;
    th_env_method_t method;
    parse_sym_entry_t *function;
    parse_sym_entry_t *rival;
    core_string_t trait;
} n00bb_dispatch_target_t;

bool n00bb_reference_dispatch(core_string_t method, bool *writable);
th_type *n00bb_integer_fit(th_ctx *types, __int128 value);
core_string_t n00bb_value_text(core_translation_unit_t *unit,
                               const ir_value_t *value, ir_span_t within);
bool n00bb_integer_range(ir_func_t *function, const ir_value_t *value,
                         const ir_instr_t *at, ir_abcd_range_t *range);
bool n00bb_declared_flagset(n00bb_link_analysis_t *analysis,
                            th_type *receiver);
parse_sym_entry_t *n00bb_flagset_flag(const n00bb_dispatch_index_t *index,
                                      th_type *receiver, core_string_t name);
parse_sym_entry_t *n00bb_case_member(const n00bb_dispatch_index_t *index,
                                     th_type *receiver, core_string_t name,
                                     bool *variant);
bool n00bb_dispatch_target(const n00bb_dispatch_index_t *index,
                           th_type *receiver, core_string_t method,
                           n00bb_dispatch_target_t *target);
bool n00bb_dispatch_target_for(const n00bb_dispatch_index_t *index,
                               th_type *receiver, core_string_t method,
                               size_t arity, n00bb_dispatch_target_t *target);
bool n00bb_assign_target(const n00bb_dispatch_index_t *index,
                         th_type *receiver, core_string_t method,
                         n00bb_dispatch_target_t *target);
typedef struct {
    ir_group_pdg_t *group;
    core_list_t(n00bb_ir_instr_ptr_t) conversions;
    core_list_t(n00bb_ir_instr_ptr_t) outcome_calls;
    n00bb_dispatch_index_t *dispatch;
    bool recovered;
} n00bb_typing_t;

bool n00bb_infer_types(n00bb_link_analysis_t *analysis,
                       core_translation_unit_t *const *units, size_t count,
                       n00bb_typing_t *typing);
bool n00bb_check_variant_arms(n00bb_link_analysis_t *analysis);
void n00bb_typing_free(n00bb_typing_t *typing);

#endif
