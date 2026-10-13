#pragma once

#include "abi_link.h"
#include "ir/ir.h"
#include "th/environment.h"

typedef struct {
    const th_env *environment;
    const ir_module_t *const *modules;
    size_t module_count;
} n00bb_abi_semantics_t;

bool n00bb_abi_link_semantic_rows(
    const n00bb_abi_selected_objects_t *selected,
    const n00bb_abi_index_plan_t *plan,
    n00bb_abi_merged_rows_t *merged, void *context);
