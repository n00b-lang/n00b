#ifndef N00BB_COMPILER_BUFFER_BORROW_H
#define N00BB_COMPILER_BUFFER_BORROW_H

#include "ir/ir.h"
#include "ir/diag.h"
#include "ir/summary.h"

enum { N00BB_FLOW_SELF, N00BB_FLOW_CONTENTS, N00BB_FLOW_KINDS };

typedef struct {
    uint64_t from[N00BB_FLOW_KINDS];
    bool     nonlocal;
    bool     fresh;
    bool     shared;
} n00bb_carried_t;

typedef struct {
    uint64_t         writes[N00BB_FLOW_KINDS];
    uint64_t         mutates[N00BB_FLOW_KINDS];
    uint64_t         escapes[N00BB_FLOW_KINDS];
    uint64_t         leaks[N00BB_FLOW_KINDS];
    uint64_t         receives;
    bool             receives_fresh;
    bool             receives_nonlocal;
    bool             writes_nonlocal;
    bool             opaque;
    uint32_t         n_carried;
    n00bb_carried_t *carried;
} n00bb_flow_t;

bool n00bb_buffer_borrow_pass(ir_module_t *module, ir_summaries_t *summaries,
                              ir_diags_t *diagnostics);
void n00bb_summary_flow(const ir_summary_t *summary, n00bb_flow_t *out);
void n00bb_flow_release(n00bb_flow_t *flow);

#endif
