#include "abi_layout.h"

#include <string.h>

#include "lib/alloc.h"
#include "lib/dict.h"
#include "ir/typelang.h"

static bool
same_final_layout(const ir_final_layout_t *layout, uint32_t storage_bits,
                  uint32_t semantic_bits, uint32_t alignment_bytes,
                  bool reserve_unused)
{
    if (layout->intrinsic_bits != storage_bits
        || layout->alignment_bytes != alignment_bytes
        || layout->n_members || layout->has_discriminant
        || layout->n_occupied != (semantic_bits != 0)
        || layout->n_reserved != (reserve_unused
                                 && storage_bits > semantic_bits)) return false;
    if (semantic_bits && (layout->occupied[0].offset != 0
                          || layout->occupied[0].width != semantic_bits))
        return false;
    if (layout->n_reserved
        && (layout->reserved[0].offset != semantic_bits
            || layout->reserved[0].width != storage_bits - semantic_bits))
        return false;
    return true;
}

static bool
publish_layout(ir_module_t *module, th_type *type,
               const core_target_desc_t *target, uint32_t semantic_bits,
               uint32_t storage_bits, uint32_t alignment_bytes,
               bool reserve_unused)
{
    if (semantic_bits > storage_bits) return false;
    const ir_final_layout_t *existing = ir_layout_final(
        module, type, target->triple);
    if (existing)
        return same_final_layout(existing, storage_bits, semantic_bits,
                                 alignment_bytes, reserve_unused);

    ir_layout_bit_run_t occupied = {.offset = 0, .width = semantic_bits};
    ir_layout_bit_run_t reserved = {.offset = semantic_bits,
                                    .width = storage_bits - semantic_bits};
    ir_final_layout_t final = {
        .intrinsic_bits = storage_bits,
        .alignment_bytes = alignment_bytes,
        .occupied = semantic_bits ? &occupied : nullptr,
        .n_occupied = semantic_bits != 0,
        .reserved = reserve_unused && reserved.width ? &reserved : nullptr,
        .n_reserved = reserve_unused && reserved.width ? 1 : 0,
    };
    return ir_layout_pin_size(module, type, storage_bits / 8)
        && ir_layout_pin_align(module, type, alignment_bytes)
        && ir_layout_publish_final(module, type, target->triple, &final);
}

static bool
publish_direct_or_alias(ir_module_t *module, th_type *type,
                        const core_target_desc_t *target, core_dict_t *seen,
                        bool *handled)
{
    *handled = false;
    type = th_resolve(type);
    ir_machine_t machine = ir_machine_of(module, type);
    uint32_t semantic_bits = machine.width;
    uint32_t storage_bits = 0;
    uint32_t alignment_bytes = 1;
    bool reserve_unused = false;
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
    else if (th_kind_of(type) == TH_SEQUENCE) {
        *handled = true;
        if (target->pointer_bits != 64
            || target->pointer_alignment_bytes != 8) return false;
        storage_bits = semantic_bits = 128;
        alignment_bytes = 8;
    }
    else if (th_kind_of(type) == TH_NAMED) {
        const char *name = nullptr;
        size_t arguments = 0;
        th_named_info(type, &name, nullptr, nullptr, &arguments);
        bool fixed_handle = name && (!strcmp(name, "string")
            || !strcmp(name, "any") || !strcmp(name, "array")
            || !strcmp(name, "view"));
        bool allocator = name && !strcmp(name, "Allocator");
        if (fixed_handle || allocator) {
            *handled = true;
            bool generic_handle = !strcmp(name, "array")
                || !strcmp(name, "view");
            const th_arg *argument = generic_handle
                ? th_named_arg(type, 0) : nullptr;
            if (arguments != (size_t)generic_handle
                || (generic_handle
                    && (!argument || argument->kind != TH_ARG_TYPE)))
                return false;
            if (allocator) {
                const core_target_scalar_t *storage =
                    core_target_integer_for_bits(target, 32);
                if (!storage || storage->width_bits != 32) return false;
                storage_bits = semantic_bits = storage->width_bits;
                alignment_bytes = storage->alignment_bytes;
            }
            else {
                if (target->pointer_bits != 64
                    || target->pointer_alignment_bytes != 8) return false;
                storage_bits = semantic_bits = 128;
                alignment_bytes = 8;
            }
            return publish_layout(module, type, target, semantic_bits,
                                  storage_bits, alignment_bytes, false);
        }
        bool atomic = name && !strcmp(name, "atomic");
        th_type *definition = ir_type_definition(module, type);
        if (!definition) return true;
        bool found = false;
        core_dict_get(seen, type, &found);
        if (found) return false;
        core_dict_put(seen, type, type);
        bool definition_handled = false;
        if (!publish_direct_or_alias(module, definition, target, seen,
                                     &definition_handled)) return false;
        if (!definition_handled) return true;
        const ir_final_layout_t *base = ir_layout_final(module, definition,
                                                        target->triple);
        if (!base || base->intrinsic_bits > UINT32_MAX) return false;
        if (base->n_occupied > 1 || base->n_reserved > 1
            || (base->n_occupied && base->occupied[0].offset)) return false;
        semantic_bits = base->n_occupied
            ? (uint32_t)base->occupied[0].width : 0;
        if (base->n_reserved
            && (base->reserved[0].offset != semantic_bits
                || base->reserved[0].width
                    != base->intrinsic_bits - semantic_bits)) return false;
        if (atomic) {
            *handled = true;
            const core_target_atomic_t *storage =
                core_target_atomic_for_bits(target, semantic_bits);
            if (!semantic_bits || !storage) return false;
            storage_bits = storage->width_bits;
            alignment_bytes = storage->alignment_bytes;
            reserve_unused = true;
        }
        else {
            storage_bits = (uint32_t)base->intrinsic_bits;
            alignment_bytes = base->alignment_bytes;
            reserve_unused = base->n_reserved != 0;
        }
        *handled = true;
    }
    else return true;
    return publish_layout(module, type, target, semantic_bits, storage_bits,
                          alignment_bytes, reserve_unused);
}

bool
n00bb_abi_publish_direct_layout(ir_module_t *module, th_type *type,
                                const core_target_desc_t *target,
                                bool *handled)
{
    if (handled) *handled = false;
    if (!module || !type || !target || !target->triple || !handled
        || strcmp(ir_module_target(module), target->triple)) return false;
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    bool ok = publish_direct_or_alias(module, type, target, &seen, handled);
    core_dict_free(&seen);
    return ok;
}

bool
n00bb_abi_publish_interned_direct_layouts(
    ir_module_t *module, const core_target_desc_t *target,
    th_type **failed_type)
{
    if (failed_type) *failed_type = nullptr;
    if (!module || !target) return false;
    for (size_t i = 0; i < ir_type_count(module); i++) {
        th_type *type = ir_type_at(module, i);
        bool handled = false;
        if (!type || !n00bb_abi_publish_direct_layout(module, type,
                                                      target, &handled)) {
            if (failed_type) *failed_type = type;
            return false;
        }
    }
    return true;
}

bool
n00bb_abi_publish_integer_like_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    uint32_t semantic_bits)
{
    if (!module || !type || !target || !target->triple || !semantic_bits
        || strcmp(ir_module_target(module), target->triple)) return false;
    const core_target_scalar_t *storage =
        core_target_integer_for_bits(target, semantic_bits);
    return storage
        && publish_layout(module, type, target, semantic_bits,
                          storage->width_bits, storage->alignment_bytes,
                          false);
}

bool
n00bb_abi_publish_c_record_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    const n00bb_abi_record_member_t *members, size_t count)
{
    if (!module || !type || !target || !target->triple || !members || !count
        || strcmp(ir_module_target(module), target->triple)
        || ir_layout_final(module, type, target->triple)) return false;

    n00bb_record_field_t *fields = core_alloc_array(n00bb_record_field_t,
                                                    count);
    n00bb_record_placement_t *placements = core_alloc_array(
        n00bb_record_placement_t, count);
    const ir_final_layout_t **children = core_alloc_array(
        const ir_final_layout_t *, count);
    ir_final_member_layout_t *final_members = core_alloc_array(
        ir_final_member_layout_t, count);
    bool ok = fields && placements && children && final_members;
    size_t occupied_count = 0, reserved_count = 0;
    for (size_t i = 0; ok && i < count; i++) {
        children[i] = ir_layout_final(module, members[i].type,
                                      target->triple);
        const ir_final_layout_t *child = children[i];
        if (!members[i].name || !members[i].type || !child
            || !members[i].c_compatible
            || child->intrinsic_bits % 8
            || child->n_occupied > SIZE_MAX - occupied_count
            || child->n_reserved > SIZE_MAX - reserved_count) {
            ok = false;
            break;
        }
        occupied_count += child->n_occupied;
        reserved_count += child->n_reserved;
        uint64_t value_bits = 0;
        for (size_t j = 0; j < child->n_occupied; j++) {
            if (child->occupied[j].width > UINT64_MAX - value_bits) {
                ok = false;
                break;
            }
            value_bits += child->occupied[j].width;
        }
        if (!ok) break;
        fields[i] = (n00bb_record_field_t){
            .normalized_name = members[i].name,
            .child_typehash = th_hash(ir_module_types(module),
                                      members[i].type),
            .declaration_ordinal = members[i].ordinal,
            .extent_bits = child->intrinsic_bits,
            .occupied_bits = value_bits,
            .alignment_bits = (uint64_t)child->alignment_bytes * 8,
            .c_compatible = true,
        };
    }

    n00bb_record_layout_t placement = {0};
    if (ok) ok = n00bb_abi_place_record(fields, count, N00BB_RECORD_C,
                                       false, placements, &placement);
    ir_layout_bit_run_t *occupied = ok && occupied_count
        ? core_alloc_array(ir_layout_bit_run_t, occupied_count) : nullptr;
    ir_layout_bit_run_t *reserved = ok && reserved_count
        ? core_alloc_array(ir_layout_bit_run_t, reserved_count) : nullptr;
    if (ok && ((occupied_count && !occupied)
               || (reserved_count && !reserved))) ok = false;

    size_t occupied_at = 0, reserved_at = 0;
    for (size_t i = 0; ok && i < count; i++) {
        const ir_final_layout_t *child = children[i];
        uint64_t base = placements[i].bit_offset;
        if (base > placement.intrinsic_bits
            || child->intrinsic_bits > placement.intrinsic_bits - base) {
            ok = false;
            break;
        }
        final_members[i] = (ir_final_member_layout_t){
            .name = members[i].name,
            .type = members[i].type,
            .ordinal = members[i].ordinal,
            .bit_offset = base,
            .occupied = occupied ? occupied + occupied_at : nullptr,
            .n_occupied = child->n_occupied,
        };
        for (size_t j = 0; j < child->n_occupied; j++) {
            occupied[occupied_at++] = (ir_layout_bit_run_t){
                .offset = base + child->occupied[j].offset,
                .width = child->occupied[j].width,
            };
        }
        for (size_t j = 0; j < child->n_reserved; j++) {
            reserved[reserved_at++] = (ir_layout_bit_run_t){
                .offset = base + child->reserved[j].offset,
                .width = child->reserved[j].width,
            };
        }
    }
    ir_final_layout_t final = {
        .intrinsic_bits = placement.intrinsic_bits,
        .alignment_bytes = (uint32_t)(placement.alignment_bits / 8),
        .occupied = occupied,
        .n_occupied = occupied_count,
        .reserved = reserved,
        .n_reserved = reserved_count,
        .members = final_members,
        .n_members = count,
    };
    if (ok && (placement.alignment_bits % 8
               || placement.alignment_bits / 8 > UINT32_MAX)) ok = false;
    if (ok) ok = ir_layout_pin_size(module, type,
                                    placement.intrinsic_bits / 8)
        && ir_layout_pin_align(module, type, final.alignment_bytes);
    for (size_t i = 0; ok && i < count; i++)
        ok = ir_layout_pin_member(module, type, members[i].name,
            placements[i].bit_offset, fields[i].extent_bits);
    if (ok) ok = ir_layout_publish_final(module, type, target->triple, &final);

    core_free(reserved);
    core_free(occupied);
    core_free(final_members);
    core_free(children);
    core_free(placements);
    core_free(fields);
    return ok;
}
