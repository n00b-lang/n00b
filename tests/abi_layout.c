#include "abi_layout.h"

#include <stdio.h>

static const ir_scalar_t SCALARS[] = {
    {"bool", IR_CLASS_BOOL, 1, IR_ROLE_NONE},
    {"char", IR_CLASS_UINT, 24, IR_ROLE_CHAR},
    {"f64", IR_CLASS_FLOAT, 64, IR_ROLE_NONE},
    {"nil", IR_CLASS_VOID, 0, IR_ROLE_NONE},
    {"u32", IR_CLASS_UINT, 32, IR_ROLE_NONE},
};

static const ir_profile_t PROFILE = {
    .name = "n00bb-abi-layout-test",
    .n_scalars = sizeof(SCALARS) / sizeof(SCALARS[0]),
    .scalars = SCALARS,
};

static int
check_scalar(ir_module_t *module, const core_target_desc_t *target,
             th_type *type, uint32_t semantic_bits, uint32_t storage_bits,
             uint32_t alignment_bytes)
{
    bool handled = false;
    if (!n00bb_abi_publish_direct_layout(module, type, target, &handled)
        || !handled || !n00bb_abi_publish_direct_layout(module, type,
                                                        target, &handled))
        return 0;
    const ir_final_layout_t *final = ir_layout_final(module, type,
                                                     target->triple);
    ir_descriptor_t *descriptor = ir_layout_of(module, type, target->triple);
    if (!final || !descriptor || final->intrinsic_bits != storage_bits
        || final->alignment_bytes != alignment_bytes
        || !ir_layout_compute(module, type, nullptr)
        || !ir_layout_is_settled(&descriptor->size)
        || descriptor->size.value != storage_bits / 8
        || !ir_layout_is_settled(&descriptor->align)
        || descriptor->align.value != alignment_bytes
        || final->n_occupied != (semantic_bits != 0)
        || final->n_reserved) return 0;
    if (semantic_bits && (final->occupied[0].offset != 0
                          || final->occupied[0].width != semantic_bits))
        return 0;
    return 1;
}

static int
check_variant(const core_target_desc_t *target)
{
    ir_ctx_t *context = ir_ctx_new();
    ir_module_t *module = ir_module_new(context, "variant", &PROFILE,
                                        target->triple);
    th_ctx *types = ir_ctx_types(context);
    th_type *wide = th_bitint(types, 64, false);
    th_type *small = th_bitint(types, 8, false);
    th_arg args[] = {
        {.kind = TH_ARG_TYPE, .type = wide},
        {.kind = TH_ARG_TYPE, .type = small},
    };
    th_type *variant = th_variant(types, "variant.Value", args, 2);
    th_member members[] = {
        {.name = "number", .type = wide},
        {.name = "small", .type = small},
    };
    n00bb_abi_variant_arm_t arms[] = {
        {.name = "number", .type = wide, .ordinal = 0},
        {.name = "small", .type = small, .ordinal = 1},
    };
    bool handled = false;
    int ok = module && variant
        && n00bb_abi_publish_direct_layout(module, wide, target, &handled)
        && handled
        && n00bb_abi_publish_direct_layout(module, small, target, &handled)
        && handled
        && ir_type_define(module, variant, th_record(types, members, 2))
        && n00bb_abi_publish_variant_layout(module, variant, target, arms, 2);
    const ir_final_layout_t *final = ok
        ? ir_layout_final(module, variant, target->triple) : nullptr;
    ok = ok && final && final->intrinsic_bits == 66
        && final->alignment_bytes == 8 && final->n_members == 2
        && final->has_discriminant && !final->discriminant_is_niche
        && final->discriminant.unit_bit_offset == 64
        && final->discriminant.unit_width_bits == 8
        && final->discriminant.lsb == 0
        && final->discriminant.value_width_bits == 2
        && final->members[0].bit_offset == 0
        && final->members[1].bit_offset == 0;

    th_type *bit1 = th_bitint(types, 1, false);
    th_type *bit2 = th_bitint(types, 2, false);
    th_arg small_args[] = {
        {.kind = TH_ARG_TYPE, .type = bit1},
        {.kind = TH_ARG_TYPE, .type = bit2},
    };
    th_type *padded = th_variant(types, "variant.Padded", small_args, 2);
    th_member small_members[] = {
        {.name = "first", .type = bit1},
        {.name = "second", .type = bit2},
    };
    n00bb_abi_variant_arm_t small_arms[] = {
        {.name = "first", .type = bit1, .ordinal = 0},
        {.name = "second", .type = bit2, .ordinal = 1},
    };
    ok = ok && n00bb_abi_publish_direct_layout(module, bit1, target,
                                                  &handled)
        && n00bb_abi_publish_direct_layout(module, bit2, target, &handled)
        && ir_type_define(module, padded, th_record(types, small_members, 2))
        && n00bb_abi_publish_variant_layout(module, padded, target,
                                            small_arms, 2);
    final = ok ? ir_layout_final(module, padded, target->triple) : nullptr;
    ok = ok && final && final->intrinsic_bits == 8
        && final->discriminant.unit_bit_offset == 0
        && final->discriminant.lsb == 2
        && final->discriminant.value_width_bits == 2;
    ir_ctx_free(context);
    return ok;
}

int main(void)
{
    static const char *triples[] = {
        "x86_64-linux", "arm64-macos", "x86_64-windows",
    };
    for (size_t i = 0; i < sizeof(triples) / sizeof(triples[0]); i++) {
        core_target_desc_t target;
        if (!core_target_profile(triples[i], 0, &target)) return 1;
        ir_ctx_t *context = ir_ctx_new();
        ir_module_t *module = ir_module_new(context, "scalar", &PROFILE,
                                            target.triple);
        th_ctx *types = ir_ctx_types(context);
        th_type *wide = th_name(types, "Wide");
        th_type *wider = th_name(types, "Wider");
        int ok = module && types
            && check_variant(&target)
            && check_scalar(module, &target, th_bitint(types, 1, true),
                            1, 8, 1)
            && check_scalar(module, &target, th_bitint(types, 9, false),
                            9, 16, 2)
            && check_scalar(module, &target, th_bits(types, 17),
                            17, 32, 4)
            && check_scalar(module, &target, th_bitint(types, 65, true),
                            65, 128, 16)
            && check_scalar(module, &target, th_name(types, "bool"),
                            1, 8, 1)
            && check_scalar(module, &target, th_name(types, "char"),
                            24, 32, 4)
            && check_scalar(module, &target, th_name(types, "f64"),
                            64, 64, 8)
            && check_scalar(module, &target, th_name(types, "nil"),
                            0, 0, 1)
            && check_scalar(module, &target, th_name(types, "string"),
                            128, 128, 8)
            && check_scalar(module, &target, th_name(types, "Allocator"),
                            32, 32, 4)
            && check_scalar(module, &target, th_name(types, "any"),
                            128, 128, 8)
            && ir_type_define(module, wide, th_bitint(types, 65, true))
            && ir_type_define(module, wider, wide)
            && check_scalar(module, &target, wider, 65, 128, 16)
            && check_scalar(module, &target, wide, 65, 128, 16);
        th_arg handle_element = {
            .kind = TH_ARG_TYPE, .type = th_name(types, "int"),
        };
        ok = ok && check_scalar(module, &target,
                th_array(types, handle_element.type),
                128, 128, 8)
            && check_scalar(module, &target,
                th_apply(types, "view", &handle_element, 1),
                128, 128, 8);
        bool handled = false;
        ok = ok && n00bb_abi_publish_direct_layout(module,
                th_name(types, "user.string"), &target, &handled)
            && !handled;
        handled = false;
        ok = ok && !n00bb_abi_publish_direct_layout(module,
                th_name(types, "array"), &target, &handled)
            && handled;
        handled = false;
        th_type *record = th_record(types, nullptr, 0);
        ok = ok && n00bb_abi_publish_direct_layout(module, record, &target,
                                                    &handled)
            && handled;
        const ir_final_layout_t *empty_record_layout = ir_layout_final(
            module, record, target.triple);
        ok = ok && empty_record_layout
            && empty_record_layout->intrinsic_bits == 0
            && empty_record_layout->alignment_bytes == 1;
        th_type *ir_only = th_bits(types, 73);
        th_type *failed_type = nullptr;
        ok = ok && ir_type_intern(module, ir_only)
            && n00bb_abi_publish_interned_direct_layouts(
                module, &target, &failed_type)
            && !failed_type;
        const ir_final_layout_t *ir_only_layout = ir_layout_final(
            module, ir_only, target.triple);
        ok = ok && ir_only_layout && ir_only_layout->intrinsic_bits == 128
            && ir_only_layout->alignment_bytes == 16
            && ir_only_layout->n_occupied == 1
            && ir_only_layout->occupied[0].width == 73;
        th_arg atomic_element = {
            .kind = TH_ARG_TYPE, .type = th_bits(types, 17),
        };
        th_type *atomic = th_apply(types, "atomic", &atomic_element, 1);
        handled = false;
        ok = ok && atomic
            && ir_type_define(module, atomic, atomic_element.type)
            && n00bb_abi_publish_direct_layout(module, atomic, &target,
                                               &handled)
            && handled;
        const ir_final_layout_t *atomic_layout = ir_layout_final(
            module, atomic, target.triple);
        ok = ok && atomic_layout && atomic_layout->intrinsic_bits == 32
            && atomic_layout->alignment_bytes == 4
            && atomic_layout->n_occupied == 1
            && atomic_layout->occupied[0].offset == 0
            && atomic_layout->occupied[0].width == 17
            && atomic_layout->n_reserved == 1
            && atomic_layout->reserved[0].offset == 17
            && atomic_layout->reserved[0].width == 15;
        th_type *atomic_alias = th_name(types, "AtomicAlias");
        handled = false;
        ok = ok && ir_type_define(module, atomic_alias, atomic)
            && n00bb_abi_publish_direct_layout(module, atomic_alias,
                                               &target, &handled)
            && handled;
        const ir_final_layout_t *alias_layout = ir_layout_final(
            module, atomic_alias, target.triple);
        ok = ok && alias_layout && alias_layout->intrinsic_bits == 32
            && alias_layout->n_occupied == 1
            && alias_layout->occupied[0].width == 17
            && alias_layout->n_reserved == 1
            && alias_layout->reserved[0].offset == 17
            && alias_layout->reserved[0].width == 15;
        th_type *unsigned_enum = th_name(types, "UnsignedEnum");
        th_type *signed_enum = th_name(types, "SignedEnum");
        th_type *wide_enum = th_name(types, "WideEnum");
        ok = ok && n00bb_abi_publish_integer_like_layout(
                module, unsigned_enum, &target, 2)
            && n00bb_abi_publish_integer_like_layout(
                module, signed_enum, &target, 3)
            && n00bb_abi_publish_integer_like_layout(
                module, wide_enum, &target, 9);
        const ir_final_layout_t *unsigned_layout = ir_layout_final(
            module, unsigned_enum, target.triple);
        const ir_final_layout_t *signed_layout = ir_layout_final(
            module, signed_enum, target.triple);
        const ir_final_layout_t *wide_layout = ir_layout_final(
            module, wide_enum, target.triple);
        ok = ok && unsigned_layout && unsigned_layout->intrinsic_bits == 8
            && unsigned_layout->occupied[0].width == 2
            && signed_layout && signed_layout->intrinsic_bits == 8
            && signed_layout->occupied[0].width == 3
            && wide_layout && wide_layout->intrinsic_bits == 16
            && wide_layout->alignment_bytes == 2
            && wide_layout->occupied[0].width == 9;
        th_type *u32 = th_name(types, "u32");
        th_type *bool_type = th_name(types, "bool");
        th_type *c_record = th_name(types, "CRecord");
        n00bb_abi_record_member_t c_members[] = {
            {.name = "count", .type = u32, .ordinal = 0,
             .c_compatible = true},
            {.name = "active", .type = bool_type, .ordinal = 1,
             .c_compatible = true},
        };
        handled = false;
        ok = ok && check_scalar(module, &target, u32, 32, 32, 4)
            && n00bb_abi_publish_c_record_layout(module, c_record, &target,
                c_members, sizeof(c_members) / sizeof(c_members[0]));
        const ir_final_layout_t *c_layout = ir_layout_final(
            module, c_record, target.triple);
        ir_descriptor_t *c_descriptor = ir_layout_of(module, c_record,
                                                      target.triple);
        ok = ok && c_layout && c_layout->intrinsic_bits == 64
            && c_layout->alignment_bytes == 4
            && c_layout->n_occupied == 2
            && c_layout->occupied[0].offset == 0
            && c_layout->occupied[0].width == 32
            && c_layout->occupied[1].offset == 32
            && c_layout->occupied[1].width == 1
            && c_layout->n_members == 2
            && c_layout->members[0].bit_offset == 0
            && c_layout->members[1].bit_offset == 32
            && c_layout->members[1].n_occupied == 1
            && c_layout->members[1].occupied[0].offset == 32
            && c_layout->members[1].occupied[0].width == 1
            && c_descriptor
            && ir_layout_compute(module, c_record, nullptr)
            && c_descriptor->size.value == 8
            && c_descriptor->align.value == 4;
        th_type *nested_record = th_name(types, "NestedCRecord");
        n00bb_abi_record_member_t nested_members[] = {
            {.name = "tag", .type = bool_type, .ordinal = 0,
             .c_compatible = true},
            {.name = "payload", .type = c_record, .ordinal = 1,
             .c_compatible = true},
        };
        ok = ok && n00bb_abi_publish_c_record_layout(module,
            nested_record, &target, nested_members,
            sizeof(nested_members) / sizeof(nested_members[0]));
        const ir_final_layout_t *nested_layout = ir_layout_final(
            module, nested_record, target.triple);
        ok = ok && nested_layout && nested_layout->intrinsic_bits == 96
            && nested_layout->alignment_bytes == 4
            && nested_layout->n_members == 2
            && nested_layout->members[0].bit_offset == 0
            && nested_layout->members[1].bit_offset == 32
            && nested_layout->n_occupied == 3
            && nested_layout->occupied[0].width == 1
            && nested_layout->occupied[1].offset == 32
            && nested_layout->occupied[1].width == 32
            && nested_layout->occupied[2].offset == 64
            && nested_layout->occupied[2].width == 1;
        th_type *rejected_record = th_name(types, "RejectedCRecord");
        c_members[1].c_compatible = false;
        ok = ok && !n00bb_abi_publish_c_record_layout(module,
            rejected_record, &target, c_members,
            sizeof(c_members) / sizeof(c_members[0]));
        th_type *bits3 = th_bits(types, 3);
        th_type *default_record = th_name(types, "DefaultRecord");
        th_type *ordered_record = th_name(types, "OrderedRecord");
        n00bb_abi_record_member_t packed_members[] = {
            {.name = "tag", .type = bool_type, .ordinal = 0,
             .packed = true},
            {.name = "count", .type = u32, .ordinal = 1},
            {.name = "bits", .type = bits3, .ordinal = 2,
             .packed = true},
        };
        handled = false;
        ok = ok && n00bb_abi_publish_direct_layout(module, bits3,
                                                     &target, &handled)
            && handled
            && n00bb_abi_publish_record_layout(module, default_record,
                &target, N00BB_RECORD_DEFAULT, false, packed_members, 3)
            && n00bb_abi_publish_record_layout(module, ordered_record,
                &target, N00BB_RECORD_ORDERED, false, packed_members, 3);
        const ir_final_layout_t *default_layout = ir_layout_final(
            module, default_record, target.triple);
        const ir_final_layout_t *ordered_layout = ir_layout_final(
            module, ordered_record, target.triple);
        ir_descriptor_t *default_descriptor = ir_layout_of(
            module, default_record, target.triple);
        ir_descriptor_t *ordered_descriptor = ir_layout_of(
            module, ordered_record, target.triple);
        ok = ok && default_layout
            && default_layout->intrinsic_bits == 36
            && default_layout->alignment_bytes == 4
            && default_layout->n_occupied == 3
            && default_layout->occupied[0].offset == 0
            && default_layout->occupied[0].width == 32
            && default_layout->occupied[1].offset == 32
            && default_layout->occupied[1].width == 3
            && default_layout->occupied[2].offset == 35
            && default_layout->occupied[2].width == 1
            && default_layout->members[0].bit_offset == 35
            && default_layout->members[0].occupied[0].offset == 35
            && default_layout->members[1].bit_offset == 0
            && default_layout->members[1].occupied[0].offset == 0
            && default_layout->members[2].bit_offset == 32
            && default_descriptor && default_descriptor->size.value == 5
            && ordered_layout && ordered_layout->intrinsic_bits == 67
            && ordered_layout->alignment_bytes == 4
            && ordered_layout->n_occupied == 3
            && ordered_layout->occupied[0].offset == 0
            && ordered_layout->occupied[1].offset == 32
            && ordered_layout->occupied[2].offset == 64
            && ordered_layout->members[0].bit_offset == 0
            && ordered_layout->members[1].bit_offset == 32
            && ordered_layout->members[2].bit_offset == 64
            && ordered_descriptor && ordered_descriptor->size.value == 9;
        th_type *empty_record = th_name(types, "EmptyRecord");
        ok = ok && n00bb_abi_publish_record_layout(module, empty_record,
                &target, N00BB_RECORD_DEFAULT, false, nullptr, 0);
        const ir_final_layout_t *empty_layout = ir_layout_final(
            module, empty_record, target.triple);
        ok = ok && empty_layout && !empty_layout->intrinsic_bits
            && empty_layout->alignment_bytes == 1
            && !empty_layout->n_members;
        th_type *aligned_empty = th_name(types, "AlignedEmpty");
        ir_final_layout_t aligned_empty_layout = {
            .intrinsic_bits = 0, .alignment_bytes = 8,
        };
        ok = ok && ir_layout_pin_size(module, aligned_empty, 0)
            && ir_layout_pin_align(module, aligned_empty, 8)
            && ir_layout_publish_final(module, aligned_empty,
                                       target.triple, &aligned_empty_layout);
        n00bb_abi_record_member_t zero_members[] = {
            {.name = "flag", .type = bool_type, .ordinal = 0,
             .packed = true},
            {.name = "empty", .type = aligned_empty, .ordinal = 1},
        };
        th_type *zero_record = th_name(types, "ZeroMemberRecord");
        ok = ok && n00bb_abi_publish_record_layout(module, zero_record,
            &target, N00BB_RECORD_ORDERED, false, zero_members, 2);
        const ir_final_layout_t *zero_layout = ir_layout_final(module,
                                                               zero_record,
                                                               target.triple);
        ok = ok && zero_layout && zero_layout->intrinsic_bits == 1
            && zero_layout->alignment_bytes == 8
            && zero_layout->n_members == 2
            && zero_layout->members[1].bit_offset == 64
            && !zero_layout->members[1].n_occupied;
        th_type *rejected_packed_c = th_name(types, "RejectedPackedC");
        packed_members[0].c_compatible = true;
        ok = ok && !n00bb_abi_publish_c_record_layout(module,
            rejected_packed_c, &target, packed_members, 3);
        ok = ok && !n00bb_abi_publish_c_record_layout(module,
            th_name(types, "EmptyCRecord"), &target, nullptr, 0);
        th_type *rejected_packed_atomic = th_name(types,
                                                   "RejectedPackedAtomic");
        packed_members[0].type = atomic;
        ok = ok && !n00bb_abi_publish_record_layout(module,
            rejected_packed_atomic, &target, N00BB_RECORD_DEFAULT,
            false, packed_members, 3);
        handled = false;
        ok = ok && !n00bb_abi_publish_direct_layout(module,
                   th_bits(types, 129), &target, &handled) && handled;
        ir_ctx_free(context);
        if (!ok) {
            fprintf(stderr, "scalar ABI layout failed for %s\n", triples[i]);
            return 1;
        }
    }
    return 0;
}
