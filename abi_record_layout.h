#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    N00BB_RECORD_DEFAULT,
    N00BB_RECORD_ORDERED,
    N00BB_RECORD_C,
} n00bb_record_policy_t;

typedef struct {
    const char *normalized_name;
    uint64_t child_typehash;
    uint32_t declaration_ordinal;
    uint64_t extent_bits;
    uint64_t occupied_bits;
    uint64_t alignment_bits;
    bool c_compatible;
} n00bb_record_field_t;

typedef struct {
    uint64_t bit_offset;
} n00bb_record_placement_t;

typedef struct {
    uint64_t intrinsic_bits;
    uint64_t alignment_bits;
} n00bb_record_layout_t;

/* The caller supplies each field's packed extent and alignment. Placements
 * remain indexed by input field; sorting changes physical order only. */
bool n00bb_abi_place_record(const n00bb_record_field_t *fields, size_t count,
                            n00bb_record_policy_t policy,
                            bool structural_record,
                            n00bb_record_placement_t *placements,
                            n00bb_record_layout_t *layout);
