#include "abi_record_layout.h"

#include <string.h>

#include "lib/alloc.h"

typedef enum {
    SORT_DECLARATION,
    SORT_ALIGNED,
    SORT_UNALIGNED,
} sort_kind_t;

static bool
power_of_two(uint64_t value)
{
    return value && !(value & (value - 1));
}

static bool
align_up(uint64_t value, uint64_t alignment, uint64_t *result)
{
    if (!power_of_two(alignment) || value > UINT64_MAX - (alignment - 1))
        return false;
    *result = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

static int
compare_fields(const n00bb_record_field_t *fields, size_t a, size_t b,
               sort_kind_t kind, bool structural)
{
    const n00bb_record_field_t *left = &fields[a];
    const n00bb_record_field_t *right = &fields[b];
    if (kind == SORT_ALIGNED
        && left->alignment_bits != right->alignment_bits)
        return left->alignment_bits > right->alignment_bits ? -1 : 1;
    if (kind != SORT_DECLARATION
        && left->occupied_bits != right->occupied_bits)
        return left->occupied_bits > right->occupied_bits ? -1 : 1;
    if (structural && kind != SORT_DECLARATION) {
        int names = core_string_cmp(left->normalized_name,
                                    right->normalized_name);
        if (names) return names;
        if (left->child_typehash != right->child_typehash)
            return left->child_typehash < right->child_typehash ? -1 : 1;
    }
    if (left->declaration_ordinal != right->declaration_ordinal)
        return left->declaration_ordinal < right->declaration_ordinal ? -1 : 1;
    return 0;
}

static void
sort_indices(const n00bb_record_field_t *fields, size_t *indices,
             size_t *scratch, size_t count, sort_kind_t kind,
             bool structural)
{
    for (size_t width = 1; width < count;) {
        for (size_t start = 0; start < count;) {
            size_t mid = count - start < width ? count : start + width;
            size_t end = count - mid < width ? count : mid + width;
            size_t left = start, right = mid;
            for (size_t out = start; out < end; out++) {
                if (right == end
                    || (left < mid && compare_fields(fields, indices[left],
                        indices[right], kind, structural) <= 0))
                    scratch[out] = indices[left++];
                else scratch[out] = indices[right++];
            }
            start = end;
        }
        memcpy(indices, scratch, count * sizeof(*indices));
        if (width > count / 2) break;
        width *= 2;
    }
}

static bool
place_at(const n00bb_record_field_t *fields, size_t index, uint64_t offset,
         n00bb_record_placement_t *placements, uint64_t *cursor,
         uint64_t *high_water)
{
    if (fields[index].extent_bits > UINT64_MAX - offset) return false;
    uint64_t end = offset + fields[index].extent_bits;
    placements[index].bit_offset = offset;
    *cursor = end;
    if (fields[index].extent_bits && end > *high_water) *high_water = end;
    return true;
}

static bool
fill_gap(const n00bb_record_field_t *fields, const size_t *unaligned,
         size_t unaligned_count, bool *placed,
         n00bb_record_placement_t *placements, uint64_t limit,
         uint64_t *cursor, uint64_t *high_water)
{
    while (*cursor < limit) {
        size_t chosen = SIZE_MAX;
        for (size_t i = 0; i < unaligned_count; i++) {
            size_t field = unaligned[i];
            if (!placed[field] && fields[field].extent_bits <= limit - *cursor) {
                chosen = field;
                break;
            }
        }
        if (chosen == SIZE_MAX) break;
        if (!place_at(fields, chosen, *cursor, placements, cursor, high_water))
            return false;
        placed[chosen] = true;
    }
    return true;
}

static bool
place_default(const n00bb_record_field_t *fields,
              const size_t *aligned, size_t aligned_count,
              const size_t *unaligned, size_t unaligned_count,
              n00bb_record_placement_t *placements, bool *placed,
              uint64_t *high_water)
{
    uint64_t cursor = 0;
    for (size_t remaining = aligned_count; remaining;) {
        size_t anchor = SIZE_MAX;
        for (size_t i = 0; i < aligned_count; i++)
            if (!placed[aligned[i]]) { anchor = aligned[i]; break; }
        uint64_t position;
        if (anchor == SIZE_MAX
            || !align_up(cursor, fields[anchor].alignment_bits, &position)
            || !fill_gap(fields, unaligned, unaligned_count, placed,
                         placements, position, &cursor, high_water)
            || !place_at(fields, anchor, position, placements, &cursor,
                         high_water)) return false;
        placed[anchor] = true;
        remaining--;
        uint64_t boundary;
        if (!align_up(cursor, fields[anchor].alignment_bits, &boundary))
            return false;
        for (;;) {
            size_t chosen = SIZE_MAX;
            uint64_t next = 0;
            for (size_t i = 0; i < aligned_count; i++) {
                size_t field = aligned[i];
                if (placed[field]) continue;
                uint64_t candidate;
                if (!align_up(cursor, fields[field].alignment_bits,
                              &candidate)) return false;
                if (candidate <= boundary
                    && fields[field].extent_bits <= boundary - candidate) {
                    chosen = field;
                    next = candidate;
                    break;
                }
            }
            if (chosen == SIZE_MAX) {
                if (!fill_gap(fields, unaligned, unaligned_count, placed,
                              placements, boundary, &cursor, high_water))
                    return false;
                cursor = boundary;
                break;
            }
            if (!fill_gap(fields, unaligned, unaligned_count, placed,
                          placements, next, &cursor, high_water)
                || !place_at(fields, chosen, next, placements, &cursor,
                             high_water)) return false;
            placed[chosen] = true;
            remaining--;
        }
    }
    for (size_t i = 0; i < unaligned_count; i++) {
        size_t field = unaligned[i];
        if (placed[field]) continue;
        if (!place_at(fields, field, cursor, placements, &cursor,
                      high_water)) return false;
    }
    return true;
}

bool
n00bb_abi_place_record(const n00bb_record_field_t *fields, size_t count,
                       n00bb_record_policy_t policy, bool structural_record,
                       n00bb_record_placement_t *placements,
                       n00bb_record_layout_t *layout)
{
    if (!layout || (count && (!fields || !placements))
        || count > SIZE_MAX / sizeof(size_t)
        || policy < N00BB_RECORD_DEFAULT || policy > N00BB_RECORD_C)
        return false;
    uint64_t alignment = 8;
    for (size_t i = 0; i < count; i++) {
        const n00bb_record_field_t *field = &fields[i];
        if (!field->normalized_name.data || !field->child_typehash
            || !power_of_two(field->alignment_bits)
            || (field->alignment_bits != 1 && field->alignment_bits < 8)
            || field->occupied_bits > field->extent_bits
            || (policy == N00BB_RECORD_C
                && (!field->c_compatible || !field->extent_bits
                    || field->extent_bits % 8
                    || field->alignment_bits < 8))) return false;
        if (field->alignment_bits > alignment)
            alignment = field->alignment_bits;
        for (size_t j = 0; j < i; j++)
            if (fields[j].declaration_ordinal == field->declaration_ordinal
                || core_string_eq(fields[j].normalized_name,
                                  field->normalized_name)) return false;
    }
    size_t *aligned = core_alloc_array(size_t, count);
    size_t *unaligned = core_alloc_array(size_t, count);
    size_t *scratch = core_alloc_array(size_t, count);
    bool *placed = core_alloc_array(bool, count);
    size_t aligned_count = 0, unaligned_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (fields[i].alignment_bits == 1 && policy == N00BB_RECORD_DEFAULT)
            unaligned[unaligned_count++] = i;
        else aligned[aligned_count++] = i;
    }
    sort_indices(fields, aligned, scratch, aligned_count,
                 policy == N00BB_RECORD_DEFAULT
                    ? SORT_ALIGNED : SORT_DECLARATION, structural_record);
    sort_indices(fields, unaligned, scratch, unaligned_count,
                 SORT_UNALIGNED, structural_record);
    uint64_t high_water = 0;
    bool ok = true;
    if (policy == N00BB_RECORD_DEFAULT)
        ok = place_default(fields, aligned, aligned_count, unaligned,
                           unaligned_count, placements, placed, &high_water);
    else {
        uint64_t cursor = 0;
        for (size_t i = 0; ok && i < aligned_count; i++) {
            size_t field = aligned[i];
            uint32_t group = fields[field].union_group;
            if (!group) {
                uint64_t offset;
                ok = align_up(cursor, fields[field].alignment_bits, &offset)
                    && place_at(fields, field, offset, placements, &cursor,
                                &high_water);
                continue;
            }
            if (i && fields[aligned[i - 1]].union_group == group) continue;
            uint64_t group_alignment = 1, offset, end;
            for (size_t j = i; j < aligned_count
                 && fields[aligned[j]].union_group == group; j++)
                if (fields[aligned[j]].alignment_bits > group_alignment)
                    group_alignment = fields[aligned[j]].alignment_bits;
            ok = align_up(cursor, group_alignment, &offset);
            end = offset;
            for (size_t j = i; ok && j < aligned_count
                 && fields[aligned[j]].union_group == group; j++) {
                uint64_t member_end = offset;
                ok = place_at(fields, aligned[j], offset, placements,
                              &member_end, &high_water);
                if (member_end > end) end = member_end;
            }
            cursor = end;
        }
        if (ok && policy == N00BB_RECORD_C)
            ok = align_up(high_water, alignment, &high_water);
    }
    uint64_t stride_bits;
    if (ok) ok = align_up(high_water, alignment, &stride_bits);
    if (ok) *layout = (n00bb_record_layout_t){
        .intrinsic_bits = high_water,
        .alignment_bits = alignment,
    };
    core_free(placed);
    core_free(scratch);
    core_free(unaligned);
    core_free(aligned);
    return ok;
}
