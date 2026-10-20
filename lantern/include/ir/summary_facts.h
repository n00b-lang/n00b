#ifndef IR_SUMMARY_FACTS_H
#define IR_SUMMARY_FACTS_H

#include "ir/summary.h"
#include "logic/datalog.h"

typedef struct ir_summary_facts_t ir_summary_facts_t;

typedef enum : uint8_t {
    IR_EFFECT_REASON_NONE,
    IR_EFFECT_REASON_OUT_ARGUMENT,
    IR_EFFECT_REASON_UNKNOWN_CALLEE,
    IR_EFFECT_REASON_CALLEE_CALLS_UNKNOWN,
    IR_EFFECT_REASON_CALLEE_WRITES,
} ir_effect_reason_kind_t;

typedef struct {
    ir_effect_reason_kind_t kind;
    const ir_func_t        *callee;
    ir_splace_t             place;
} ir_effect_reason_t;

ir_summary_facts_t *ir_summary_facts_build(ir_summaries_t *summaries,
                                           ir_func_t *const *functions,
                                           size_t count, bool provenance);
void                ir_summary_facts_free(ir_summary_facts_t *facts);
core_datalog_engine_t *ir_summary_facts_engine(ir_summary_facts_t *facts);
bool ir_summary_facts_side_effect(ir_summary_facts_t *facts,
                                  const ir_instr_t *call);
bool ir_summary_facts_ignored_result(ir_summary_facts_t *facts,
                                     const ir_instr_t *call);
bool ir_summary_facts_explain_side_effect(ir_summary_facts_t *facts,
                                          const ir_instr_t *call,
                                          ir_effect_reason_t *reason);

#endif
