#ifndef TYPES_CONSTRAIN_H
#define TYPES_CONSTRAIN_H

#include "ir/pdg.h"
#include "lib/list.h"
#include "types/solver.h"

typedef ir_instr_t *ty_outcome_call_t;

core_list_decl(ty_outcome_call_t);

typedef struct {
    size_t                         instructions;
    size_t                         skipped;
    uint32_t                       skipped_by_op[IR_OP_COUNT];
    core_list_t(ty_outcome_call_t) outcome_calls;
} ty_coverage_t;

bool ty_constrain_group(ty_solver_t *s, const ir_group_pdg_t *g,
                        ty_coverage_t *coverage);

#endif
