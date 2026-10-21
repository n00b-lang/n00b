#ifndef IR_SUMMARY_FACTS_H
#define IR_SUMMARY_FACTS_H

#include "ir/summary.h"
#include "logic/datalog.h"

typedef struct ir_summary_facts_t ir_summary_facts_t;

typedef enum : uint8_t {
    IR_SUMMARY_STEP_RULE,
    IR_SUMMARY_STEP_OUT_ARGUMENT,
    IR_SUMMARY_STEP_DISCARDED,
    IR_SUMMARY_STEP_CALLS,
    IR_SUMMARY_STEP_CALLS_UNSEEN,
    IR_SUMMARY_STEP_FLAG,
    IR_SUMMARY_STEP_ACCESS,
    IR_SUMMARY_STEP_ESCAPE,
    IR_SUMMARY_STEP_ADDRESSABLE,
    IR_SUMMARY_STEP_PLACE,
    IR_SUMMARY_STEP_RETURNS,
    IR_SUMMARY_STEP_DEPENDS,
} ir_summary_step_kind_t;

typedef struct {
    ir_summary_step_kind_t kind;
    int32_t                depth;
    core_string_t          relation;
    const ir_func_t       *callee;
    core_string_t          name;
    uint32_t               param;
    ir_splace_t            place;
} ir_summary_step_t;

typedef bool (*ir_summary_explain_fn)(void *context,
                                      const ir_summary_step_t *step);

ir_summary_facts_t *ir_summary_facts_build(ir_summaries_t *summaries,
                                           ir_func_t *const *functions,
                                           size_t count, bool provenance);
void                ir_summary_facts_free(ir_summary_facts_t *facts);
core_datalog_engine_t *ir_summary_facts_engine(ir_summary_facts_t *facts);
bool ir_summary_facts_side_effect(ir_summary_facts_t *facts,
                                  const ir_instr_t *call);
bool ir_summary_facts_ignored_result(ir_summary_facts_t *facts,
                                     const ir_instr_t *call);
bool ir_summary_facts_call_diverges(ir_summary_facts_t *facts,
                                    const ir_instr_t *call);
bool ir_summary_facts_call_reads_entry(ir_summary_facts_t *facts,
                                       const ir_instr_t *call,
                                       const ir_splace_t *place);
uint8_t ir_summary_facts_field_use(ir_summary_facts_t *facts,
                                   core_string_t record, core_string_t field);
bool ir_summary_facts_explain_side_effect(ir_summary_facts_t *facts,
                                          const ir_instr_t *call,
                                          ir_summary_explain_fn visit,
                                          void *context);
bool ir_summary_facts_explain_divergence(ir_summary_facts_t *facts,
                                         const ir_instr_t *call,
                                         ir_summary_explain_fn visit,
                                         void *context);
bool ir_summary_facts_explain_ignored_result(ir_summary_facts_t *facts,
                                             const ir_instr_t *call,
                                             ir_summary_explain_fn visit,
                                             void *context);

#endif
