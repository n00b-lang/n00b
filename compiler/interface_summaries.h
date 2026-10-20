#ifndef N00BB_INTERFACE_SUMMARIES_H
#define N00BB_INTERFACE_SUMMARIES_H

#include "compiler_link.h"
#include "ir/summary.h"

typedef struct n00bb_interface_summaries_t n00bb_interface_summaries_t;

bool n00bb_publish_function_summaries(n00bb_link_analysis_t *analysis);
ir_summary_t *n00bb_interface_summary(n00bb_link_analysis_t *analysis,
                                      ir_summaries_t *summaries,
                                      const ir_func_t *function);
void n00bb_interface_summaries_free(n00bb_interface_summaries_t *index);

#endif
