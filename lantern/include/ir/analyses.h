#ifndef IR_ANALYSES_H
#define IR_ANALYSES_H

#include "ir/doms.h"
#include "ir/ir.h"
#include "ir/memssa.h"
#include "ir/pdg.h"
#include "ir/places.h"
#include "ir/ranges.h"
#include "ir/sites.h"

ir_doms_t   *ir_func_doms(ir_func_t *f);
ir_pdg_t    *ir_func_pdg(ir_func_t *f);
ir_ranges_t *ir_func_ranges(ir_func_t *f);
void         ir_func_release_analyses(ir_func_t *f);
void         ir_module_release_analyses(ir_module_t *m);

size_t       ir_analysis_scope_begin(void);
void         ir_analysis_scope_end(size_t scope);

typedef struct ir_summaries_t ir_summaries_t;
ir_summaries_t *ir_analysis_bind_summaries(ir_summaries_t *summaries);
ir_summaries_t *ir_analysis_summaries(void);

#endif
