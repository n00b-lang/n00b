#ifndef N00BB_COMPILER_BUFFER_SLICES_H
#define N00BB_COMPILER_BUFFER_SLICES_H

#include "ir/ir.h"
#include "ir/diag.h"
#include "ir/summary.h"

enum { N00BB_FLOW_SELF, N00BB_FLOW_CONTENTS, N00BB_FLOW_KINDS };

typedef struct {
    uint64_t writes[N00BB_FLOW_KINDS];
    uint64_t mutates[N00BB_FLOW_KINDS];
    uint64_t escapes[N00BB_FLOW_KINDS];
    uint64_t leaks[N00BB_FLOW_KINDS];
    bool     writes_nonlocal;
    bool     opaque;
} n00bb_flow_t;

bool n00bb_buffer_slice_pass(ir_module_t *module, ir_summaries_t *summaries,
                             ir_diags_t *diagnostics);
#define N00BB_SLICE_COPY_ANNOTATION "n00bb.slice_copy"

static inline bool
n00bb_buffer_slice_copies(ir_instr_t *conversion)
{
    return conversion && ir_annot_get(&conversion->annots,
        CORE_STRING_STATIC(N00BB_SLICE_COPY_ANNOTATION));
}
void n00bb_summary_flow(const ir_summary_t *summary, n00bb_flow_t *out);

#endif
