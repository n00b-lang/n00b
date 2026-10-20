#pragma once

#include "lib/target.h"

typedef enum {
    CORE_TARGET_ATOM_INTEGER,
    CORE_TARGET_ATOM_FLOAT,
} core_target_atom_kind_t;

/* Flatten nested aggregates into physical scalar leaves. For an integer
 * bitfield, alignment_bits may be 1; for an ordinary leaf it is the leaf's
 * natural alignment. Overlapping leaves describe union alternatives. */
typedef struct {
    uint64_t offset_bits;
    uint64_t width_bits;
    uint32_t alignment_bits;
    core_target_atom_kind_t kind;
} core_target_call_atom_t;

typedef struct {
    uint64_t size_bytes;
    uint32_t alignment_bytes;
    const core_target_call_atom_t *atoms;
    size_t atom_count;
} core_target_aggregate_t;

typedef enum {
    CORE_TARGET_TRANSPORT_IGNORE,
    CORE_TARGET_TRANSPORT_DIRECT,
    CORE_TARGET_TRANSPORT_STACK,
    CORE_TARGET_TRANSPORT_INDIRECT,
} core_target_transport_t;

typedef enum {
    CORE_TARGET_LANE_NONE,
    CORE_TARGET_LANE_INTEGER,
    CORE_TARGET_LANE_FLOAT,
} core_target_lane_t;

typedef struct {
    core_target_transport_t transport;
    uint8_t lane_count;
    core_target_lane_t lanes[4];
} core_target_aggregate_class_t;

/* Classify one aggregate's transport, not its register allocation. The
 * caller supplies the final byte size, alignment, and flattened physical
 * leaves. force_indirect lets the calling language impose its own transport
 * rule before target classification. The
 * separate n00b scalar i:128/u:128 rule is not an aggregate classification.
 * Invalid or unsupported inputs fail without modifying out. */
bool core_target_classify_aggregate(const core_target_desc_t *target,
                                    const core_target_aggregate_t *aggregate,
                                    bool force_indirect, bool is_return,
                                    core_target_aggregate_class_t *out);
