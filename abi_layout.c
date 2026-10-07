#include "abi_layout.h"

#include <string.h>

#include "lib/dict.h"

static bool
same_final_layout(const ir_final_layout_t *layout, uint32_t storage_bits,
                  uint32_t semantic_bits, uint32_t alignment_bytes)
{
    if (layout->intrinsic_bits != storage_bits
        || layout->alignment_bytes != alignment_bytes
        || layout->n_members || layout->has_discriminant
        || layout->n_occupied != (semantic_bits != 0)
        || layout->n_reserved) return false;
    if (semantic_bits && (layout->occupied[0].offset != 0
                          || layout->occupied[0].width != semantic_bits))
        return false;
    return true;
}

static bool
publish_layout(ir_module_t *module, th_type *type,
               const core_target_desc_t *target, uint32_t semantic_bits,
               uint32_t storage_bits, uint32_t alignment_bytes)
{
    const ir_final_layout_t *existing = ir_layout_final(
        module, type, target->triple);
    if (existing)
        return same_final_layout(existing, storage_bits, semantic_bits,
                                 alignment_bytes);

    ir_layout_bit_run_t occupied = {.offset = 0, .width = semantic_bits};
    ir_final_layout_t final = {
        .intrinsic_bits = storage_bits,
        .alignment_bytes = alignment_bytes,
        .occupied = semantic_bits ? &occupied : nullptr,
        .n_occupied = semantic_bits != 0,
    };
    return ir_layout_pin_size(module, type, storage_bits / 8)
        && ir_layout_pin_align(module, type, alignment_bytes)
        && ir_layout_publish_final(module, type, target->triple, &final);
}

static bool
publish_scalar_or_alias(ir_module_t *module, th_type *type,
                        const core_target_desc_t *target, core_dict_t *seen,
                        bool *handled)
{
    *handled = false;
    type = th_resolve(type);
    ir_machine_t machine = ir_machine_of(module, type);
    uint32_t semantic_bits = machine.width;
    uint32_t storage_bits = 0;
    uint32_t alignment_bytes = 1;
    if (machine.class == IR_CLASS_VOID) {
        *handled = true;
        if (semantic_bits) return false;
    }
    else if (machine.class == IR_CLASS_SINT
             || machine.class == IR_CLASS_UINT
             || machine.class == IR_CLASS_BOOL) {
        *handled = true;
        if (!semantic_bits || semantic_bits > 128
            || (machine.class == IR_CLASS_BOOL && semantic_bits != 1))
            return false;
        const core_target_scalar_t *storage =
            core_target_integer_for_bits(target, semantic_bits);
        if (!storage) return false;
        storage_bits = storage->width_bits;
        alignment_bytes = storage->alignment_bytes;
    }
    else if (machine.class == IR_CLASS_FLOAT) {
        *handled = true;
        if (semantic_bits != 64) return false;
        const core_target_scalar_t *storage =
            core_target_float_storage(target, semantic_bits);
        if (!storage) return false;
        storage_bits = storage->width_bits;
        alignment_bytes = storage->alignment_bytes;
    }
    else if (th_kind_of(type) == TH_NAMED) {
        th_type *definition = ir_type_definition(module, type);
        if (!definition) return true;
        bool found = false;
        core_dict_get(seen, type, &found);
        if (found) return false;
        core_dict_put(seen, type, type);
        bool definition_handled = false;
        if (!publish_scalar_or_alias(module, definition, target, seen,
                                     &definition_handled)) return false;
        if (!definition_handled) return true;
        const ir_final_layout_t *base = ir_layout_final(module, definition,
                                                        target->triple);
        if (!base || base->intrinsic_bits > UINT32_MAX) return false;
        semantic_bits = base->n_occupied
            ? (uint32_t)base->occupied[0].width : 0;
        storage_bits = (uint32_t)base->intrinsic_bits;
        alignment_bytes = base->alignment_bytes;
        *handled = true;
    }
    else return true;
    return publish_layout(module, type, target, semantic_bits, storage_bits,
                          alignment_bytes);
}

bool
n00bb_abi_publish_scalar_layout(ir_module_t *module, th_type *type,
                                const core_target_desc_t *target,
                                bool *handled)
{
    if (handled) *handled = false;
    if (!module || !type || !target || !target->triple || !handled
        || strcmp(ir_module_target(module), target->triple)) return false;
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    bool ok = publish_scalar_or_alias(module, type, target, &seen, handled);
    core_dict_free(&seen);
    return ok;
}
