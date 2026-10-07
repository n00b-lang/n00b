#include "abi_layout.h"

#include <stdio.h>

static const ir_scalar_t SCALARS[] = {
    {"bool", IR_CLASS_BOOL, 1, IR_ROLE_NONE},
    {"char", IR_CLASS_UINT, 24, IR_ROLE_CHAR},
    {"f64", IR_CLASS_FLOAT, 64, IR_ROLE_NONE},
    {"nil", IR_CLASS_VOID, 0, IR_ROLE_NONE},
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
            && !handled;
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
