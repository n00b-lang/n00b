#ifndef N00BB_INTERFACE_SUMMARIES_H
#define N00BB_INTERFACE_SUMMARIES_H

#include "compiler_link.h"
#include "ir/summary.h"

typedef struct n00bb_interface_summaries_t n00bb_interface_summaries_t;

typedef bool (*n00bb_module_encode_fn)(void *context, ir_module_t *module,
                                       core_buffer_t *out);
typedef bool (*n00bb_module_metadata_fn)(void *context, core_string_t module,
                                         const uint8_t *data, size_t size);

bool n00bb_publish_module_metadata(n00bb_link_analysis_t *analysis,
                                   core_string_t owner_prefix,
                                   core_string_t key, uint32_t version,
                                   n00bb_module_encode_fn encode,
                                   void *context);
bool n00bb_each_module_metadata(const th_env *environment,
                                core_string_t owner_prefix, core_string_t key,
                                uint32_t version,
                                n00bb_module_metadata_fn visit, void *context);
bool n00bb_publish_function_summaries(n00bb_link_analysis_t *analysis);
ir_summary_t *n00bb_interface_summary(n00bb_link_analysis_t *analysis,
                                      ir_summaries_t *summaries,
                                      const ir_func_t *function);
bool n00bb_encode_portable_summaries(ir_summaries_t *summaries,
                                     ir_module_t *module, core_buffer_t *out);
ir_summary_t *n00bb_stored_summary(void *context, ir_summaries_t *summaries,
                                   const ir_func_t *function);
void n00bb_interface_summaries_free(n00bb_interface_summaries_t *index);

#endif
