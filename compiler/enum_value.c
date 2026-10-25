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

void
n00bb_enum_range_finish(n00bb_enum_range_t *range)
{
    if (range->explicit_storage) return;
    uint64_t needed = range->magnitude_bits + range->is_signed;
    if (!needed) needed = 1;
    range->storage_signed = range->is_signed;
    if (!range->is_open) {
        range->storage_bits = needed > UINT32_MAX ? UINT32_MAX
                                                  : (uint32_t)needed;
        return;
    }
    uint32_t width = 8;
    while (width < needed && width < 128) width *= 2;
    range->storage_bits = needed > width ? (uint32_t)needed : width;
}

bool
n00bb_enum_members_fit(const n00bb_enum_range_t *range, uint32_t bits,
                       bool is_signed)
{
    if (!is_signed) return !range->is_signed && range->magnitude_bits <= bits;
    return range->magnitude_bits + 1 <= bits;
}

uint64_t
n00bb_enum_value_bits(const n00bb_enum_range_t *range)
{
    if (range->is_open) return range->storage_bits;
    uint64_t needed = range->magnitude_bits + range->is_signed;
    return needed ? needed : 1;
}

bool
n00bb_enum_value_signed(const n00bb_enum_range_t *range)
{
    return range->is_open ? range->storage_signed : range->is_signed;
}

bool
n00bb_bind_enum_range(th_ctx *types, core_arena_t *arena,
                      parse_sym_entry_t *symbol, n00bb_enum_range_t range)
{
    n00bb_enum_range_finish(&range);
    n00bb_enum_range_t *stored = core_arena_alloc(arena, sizeof(*stored));
    if (!stored) return false;
    *stored = range;
    if (!parse_sym_set_binding(symbol, CORE_STRING_STATIC("n00bb.enum-range"),
                               stored)) return false;
    if (range.storage_bits > 128) return true;
    th_type *representation = th_bitint(types, range.storage_bits,
                                        range.storage_signed);
    return representation && th_type_define(types, symbol->type,
                                            representation);
}
