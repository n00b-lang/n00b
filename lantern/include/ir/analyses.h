#ifndef IR_ANALYSES_H
#define IR_ANALYSES_H

#include "ir/abcd.h"
#include "ir/doms.h"
#include "ir/ir.h"
#include "ir/pdg.h"
#include "ir/places.h"
#include "ir/sites.h"
#include "ir/vn.h"

ir_doms_t   *ir_func_doms(ir_func_t *f);
ir_pdg_t    *ir_func_pdg(ir_func_t *f);
ir_pdg_t    *ir_func_control(ir_func_t *f);
void         ir_func_release_analyses(ir_func_t *f);
uint64_t     ir_func_shape(const ir_func_t *f);
uint64_t     ir_func_identity(const ir_func_t *f);
void         ir_module_release_analyses(ir_module_t *m);

size_t       ir_analysis_scope_begin(void);
void         ir_analysis_scope_end(size_t scope);
void         ir_analysis_read_only_begin(void);
void         ir_analysis_read_only_end(void);

typedef struct ir_summaries_t ir_summaries_t;
ir_summaries_t *ir_analysis_bind_summaries(ir_summaries_t *summaries);
ir_summaries_t *ir_analysis_summaries(void);

// What a function's analyses asked of the bound summaries, with the
// answers, so a summary computed with them can tell whether computing it
// again with other summaries bound would come out the same.
typedef enum : uint8_t {
    IR_ANALYSIS_QUERY_BODY,   // `function`'s identity
    IR_ANALYSIS_QUERY_PURE,   // ir_summary_call_pure(call)
    IR_ANALYSIS_QUERY_WRITES, // ir_summary_call_writes(function, call, index)
    IR_ANALYSIS_QUERY_ESCAPE, // `function`'s summary and parameter `index`
} ir_analysis_query_kind_t;

typedef struct {
    ir_analysis_query_kind_t kind;
    uint32_t                 index;
    ir_func_t               *function;
    const ir_instr_t        *call;
    uint64_t                 answer;
} ir_analysis_query_t;
core_list_decl(ir_analysis_query_t);

void ir_analysis_note(ir_func_t *into, ir_analysis_query_t query);
void ir_analysis_note_pure(ir_func_t *into, const ir_instr_t *call,
                           bool answer);
void ir_analysis_note_callee(ir_func_t *into, ir_func_t *callee);
bool ir_analysis_queries(ir_func_t *f, uint64_t identity,
                         core_list_t(ir_analysis_query_t) *out);
uint64_t ir_analysis_query_answer(ir_summaries_t *summaries,
                                  const ir_analysis_query_t *query);

#endif
