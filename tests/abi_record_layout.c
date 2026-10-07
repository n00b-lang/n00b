#include "abi_record_layout.h"

#include <stdio.h>

static int
expect(const char *label, const n00bb_record_field_t *fields, size_t count,
       n00bb_record_policy_t policy, bool structural,
       uint64_t size, uint64_t alignment, const uint64_t *offsets)
{
    n00bb_record_placement_t placed[8] = {0};
    n00bb_record_layout_t layout = {0};
    if (count > sizeof(placed) / sizeof(placed[0])
        || !n00bb_abi_place_record(fields, count, policy, structural,
                                   placed, &layout)
        || layout.intrinsic_bits != size
        || layout.alignment_bits != alignment) {
        fprintf(stderr, "%s: size/alignment mismatch\n", label);
        return 0;
    }
    for (size_t i = 0; i < count; i++)
        if (placed[i].bit_offset != offsets[i]) {
            fprintf(stderr, "%s: field %zu at %llu, wanted %llu\n",
                    label, i, (unsigned long long)placed[i].bit_offset,
                    (unsigned long long)offsets[i]);
            return 0;
        }
    return 1;
}

int main(void)
{
    static const n00bb_record_field_t first[] = {
        {"a", 1, 0, 64, 64, 64, false},
        {"b", 2, 1, 24, 24, 32, false},
        {"c", 3, 2, 8, 8, 8, false},
        {"d", 4, 3, 3, 3, 1, false},
        {"e", 5, 4, 1, 1, 1, false},
    };
    static const uint64_t first_offsets[] = {0, 64, 88, 96, 99};
    static const n00bb_record_field_t second[] = {
        {"a", 1, 0, 40, 40, 64, false},
        {"b", 2, 1, 16, 16, 16, false},
        {"c", 3, 2, 8, 8, 8, false},
        {"d", 4, 3, 5, 5, 1, false},
        {"e", 5, 4, 1, 1, 1, false},
    };
    static const uint64_t second_offsets[] = {0, 48, 64, 40, 45};
    static const n00bb_record_field_t zero[] = {
        {"z", 1, 0, 0, 0, 64, false},
        {"b", 2, 1, 8, 8, 8, false},
    };
    static const uint64_t zero_offsets[] = {0, 0};
    static const n00bb_record_field_t tail[] = {
        {"a", 1, 0, 40, 40, 64, false},
    };
    static const uint64_t tail_offsets[] = {0};
    static const n00bb_record_field_t ties[] = {
        {"z", 1, 0, 8, 8, 8, false},
        {"a", 2, 1, 8, 8, 8, false},
    };
    static const uint64_t named_offsets[] = {0, 8};
    static const uint64_t structural_offsets[] = {8, 0};
    static const n00bb_record_field_t ordered[] = {
        {"a", 1, 0, 8, 8, 8, false},
        {"b", 2, 1, 40, 40, 64, false},
        {"c", 3, 2, 3, 3, 1, false},
    };
    static const uint64_t ordered_offsets[] = {0, 64, 104};
    static const n00bb_record_field_t c_fields[] = {
        {"a", 1, 0, 8, 8, 8, true},
        {"b", 2, 1, 64, 64, 64, true},
    };
    static const uint64_t c_offsets[] = {0, 64};
    static const n00bb_record_field_t c_tail[] = {
        {"a", 1, 0, 64, 64, 64, true},
        {"b", 2, 1, 8, 8, 8, true},
    };
    static const n00bb_record_field_t non_c_field[] = {
        {"value", 1, 0, 128, 128, 64, false},
    };
    static const n00bb_record_field_t overflow[] = {
        {"huge", 1, 0, UINT64_MAX, UINT64_MAX, 1, false},
    };
    static const n00bb_record_field_t duplicate[] = {
        {"x", 1, 0, 8, 8, 8, false},
        {"x", 2, 1, 8, 8, 8, false},
    };
    n00bb_record_layout_t result = {0};
    n00bb_record_placement_t placed[8] = {0};
    return !(expect("default vector one", first, 5, N00BB_RECORD_DEFAULT,
                    false, 100, 64, first_offsets)
        && expect("default vector two", second, 5, N00BB_RECORD_DEFAULT,
                  false, 72, 64, second_offsets)
        && expect("zero-sized field", zero, 2, N00BB_RECORD_DEFAULT,
                  false, 8, 64, zero_offsets)
        && expect("tail reuse", tail, 1, N00BB_RECORD_DEFAULT,
                  false, 40, 64, tail_offsets)
        && expect("named tie", ties, 2, N00BB_RECORD_DEFAULT,
                  false, 16, 8, named_offsets)
        && expect("structural tie", ties, 2, N00BB_RECORD_DEFAULT,
                  true, 16, 8, structural_offsets)
        && expect("ordered layout", ordered, 3, N00BB_RECORD_ORDERED,
                  false, 107, 64, ordered_offsets)
        && expect("C layout", c_fields, 2, N00BB_RECORD_C,
                  false, 128, 64, c_offsets)
        && expect("C tail padding", c_tail, 2, N00BB_RECORD_C,
                  false, 128, 64, c_offsets)
        && expect("empty record", nullptr, 0, N00BB_RECORD_DEFAULT,
                  false, 0, 8, nullptr)
        && !n00bb_abi_place_record(overflow, 1, N00BB_RECORD_DEFAULT,
                                   false, placed, &result)
        && !n00bb_abi_place_record(duplicate, 2, N00BB_RECORD_DEFAULT,
                                   false, placed, &result)
        && !n00bb_abi_place_record(non_c_field, 1, N00BB_RECORD_C,
                                   false, placed, &result)
        && !n00bb_abi_place_record(ordered, 3, N00BB_RECORD_C,
                                   false, placed, &result));
}
