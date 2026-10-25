#pragma once

#include "ir/ir.h"
#include "lib/arena.h"
#include "parse/symtab.h"

typedef struct {
    ir_uint_t magnitude;
    bool negative;
} n00bb_enum_value_t;

// What an enum's values occupy.
//
// `magnitude_bits` and `is_signed` describe the declared members only.
// `storage_bits` and `storage_signed` are the representation every value
// is stored in: the declared `: T`, or the best fit chosen by
// `n00bb_enum_range_finish`. A closed enum's values are its members; an
// open enum's values are every value of its storage.
typedef struct {
    uint64_t magnitude_bits;
    bool is_signed;
    bool is_open;
    bool explicit_storage;
    bool storage_signed;
    uint32_t storage_bits;
} n00bb_enum_range_t;

void n00bb_enum_range_include(n00bb_enum_range_t *range,
                              n00bb_enum_value_t value);
// Chooses the storage of an enum without a declared `: T`: the exact bits
// the members need for a closed enum, and the smallest of 8, 16, 32, 64 or
// 128 bits for an open one.
void n00bb_enum_range_finish(n00bb_enum_range_t *range);
// True when every member fits in `bits` bits of the given signedness.
bool n00bb_enum_members_fit(const n00bb_enum_range_t *range, uint32_t bits,
                            bool is_signed);
// The bits and signedness an enum's values need when converted to an
// integer: the members for a closed enum, the storage for an open one.
uint64_t n00bb_enum_value_bits(const n00bb_enum_range_t *range);
bool n00bb_enum_value_signed(const n00bb_enum_range_t *range);
bool n00bb_bind_enum_range(th_ctx *types, core_arena_t *arena,
                           parse_sym_entry_t *symbol,
                           n00bb_enum_range_t range);
