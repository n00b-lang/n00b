#include "enum_value.h"

void
n00bb_enum_range_include(n00bb_enum_range_t *range, n00bb_enum_value_t value)
{
    uint64_t bits = 0;
    for (ir_uint_t magnitude = value.magnitude; magnitude; magnitude >>= 1)
        bits++;
    if (bits > range->magnitude_bits) range->magnitude_bits = bits;
    range->is_signed |= value.negative;
}

bool
n00bb_bind_enum_range(th_ctx *types, core_arena_t *arena,
                      parse_sym_entry_t *symbol, n00bb_enum_range_t range)
{
    n00bb_enum_range_t *stored = core_arena_alloc(arena, sizeof(*stored));
    if (!stored) return false;
    *stored = range;
    if (!parse_sym_set_binding(symbol, CORE_STRING_STATIC("n00bb.enum-range"),
                               stored)) return false;
    uint64_t width = range.magnitude_bits + range.is_signed;
    if (!width) width = 1;
    if (width > 128) return true;
    th_type *representation = th_bitint(types, (uint32_t)width,
                                        range.is_signed);
    return representation && th_type_define(types, symbol->type,
                                            representation);
}
