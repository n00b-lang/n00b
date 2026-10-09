#pragma once

#include "ir/ir.h"

typedef struct ir_storage_origin_t ir_storage_origin_t;
typedef bool (*ir_mapping_base_source_fn)(const ir_value_t *value,
                                           void *context);
typedef bool (*ir_unmanaged_parameter_source_fn)(const ir_value_t *parameter,
                                                  void *context);
typedef bool (*ir_closed_callers_fn)(const ir_func_t *function, void *context);

/* A source identifies a raw mapping with an uncollected base object. A typed
 * bitcast of its base may be proven unmanaged; byte offsets cannot. Unknown
 * origins and indirect callers remain unproven. Set closed_field_writes only
 * when every mutation of the candidate fields is visible as a direct field
 * store in reachable IR; whole-object and raw-address writes must be absent. */
ir_storage_origin_t *ir_storage_origin_new(
    ir_func_t *const *reachable, size_t count,
    ir_mapping_base_source_fn source,
    ir_unmanaged_parameter_source_fn unmanaged_parameter,
    ir_closed_callers_fn closed_callers, bool closed_field_writes,
    void *context);
bool ir_storage_origin_unmanaged(ir_storage_origin_t *analysis,
                                 const ir_value_t *address);
void ir_storage_origin_free(ir_storage_origin_t *analysis);
