#include "abi_layout.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "ir/typelang.h"

core_list_decl(ir_layout_bit_run_t);

static bool publish_tuple_layout(ir_module_t *module, th_type *type,
                                 const core_target_desc_t *target,
                                 core_dict_t *seen, bool *handled);
static bool publish_structural_record_layout(ir_module_t *module,
                                             th_type *type,
                                             const core_target_desc_t *target,
                                             core_dict_t *seen,
                                             bool *handled);
static bool publish_direct_or_alias(ir_module_t *module, th_type *type,
                                    const core_target_desc_t *target,
                                    core_dict_t *seen, bool *handled);
static int compare_layout_runs(const void *left, const void *right);

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
option_presence_bit(const ir_final_layout_t *child, uint64_t *bit)
{
    core_list_t(ir_layout_bit_run_t) blocked = {0};
    for (size_t i = 0; i < child->n_occupied; i++)
        core_list_push(blocked, child->occupied[i]);
    for (size_t i = 0; i < child->n_reserved; i++)
        core_list_push(blocked, child->reserved[i]);
    qsort(blocked.data, blocked.len, sizeof(*blocked.data),
          compare_layout_runs);
    uint64_t cursor = child->intrinsic_bits;
    bool reused = false;
    for (size_t i = blocked.len; i > 0; i--) {
        const ir_layout_bit_run_t *run = &blocked.data[i - 1];
        uint64_t end = run->offset + run->width;
        if (end < cursor) {
            *bit = cursor - 1;
            reused = true;
            break;
        }
        cursor = run->offset;
    }
    if (!reused && cursor) {
        *bit = cursor - 1;
        reused = true;
    }
    if (!reused) *bit = child->intrinsic_bits;
    core_list_free(blocked);
    return true;
}

static bool
same_option_layout(const ir_final_layout_t *a,
                   const ir_final_layout_t *b)
{
    if (a->intrinsic_bits != b->intrinsic_bits
        || a->alignment_bytes != b->alignment_bytes
        || a->n_occupied != b->n_occupied
        || a->n_reserved != b->n_reserved
        || a->n_members || !a->has_discriminant
        || a->discriminant_is_niche != b->discriminant_is_niche
        || a->discriminant.unit_bit_offset
            != b->discriminant.unit_bit_offset
        || a->discriminant.unit_width_bits
            != b->discriminant.unit_width_bits
        || a->discriminant.lsb != b->discriminant.lsb
        || a->discriminant.value_width_bits
            != b->discriminant.value_width_bits) return false;
    for (size_t i = 0; i < a->n_occupied; i++)
        if (a->occupied[i].offset != b->occupied[i].offset
            || a->occupied[i].width != b->occupied[i].width) return false;
    for (size_t i = 0; i < a->n_reserved; i++)
        if (a->reserved[i].offset != b->reserved[i].offset
            || a->reserved[i].width != b->reserved[i].width) return false;
    return true;
}

static bool
publish_option_layout(ir_module_t *module, th_type *type,
                      const core_target_desc_t *target, core_dict_t *seen,
                      bool *handled)
{
    const th_arg *argument = th_named_arg(type, 0);
    if (th_named_nargs(type) != 1 || !argument
        || argument->kind != TH_ARG_TYPE) return false;
    if (!th_is_closed(ir_module_types(module), type)) return true;
    bool child_handled = false;
    if (!publish_direct_or_alias(module, argument->type, target, seen,
                                 &child_handled)) return false;
    const ir_final_layout_t *child = ir_layout_final(module, argument->type,
                                                    target->triple);
    if (!child) return true;
    bool pointer_niche = th_kind_of(th_resolve(argument->type))
            == TH_INDIRECTION && child->intrinsic_bits == 64;
    bool bitptr_niche = th_kind_of(th_resolve(argument->type))
            == TH_INDIRECTION && child->intrinsic_bits == 128;
    if (!bitptr_niche && child->has_discriminant
        && child->discriminant_is_niche
        && child->discriminant.unit_bit_offset == 64)
        bitptr_niche = true;
    uint64_t presence_bit = 0;
    if (!pointer_niche && !bitptr_niche
        && !option_presence_bit(child, &presence_bit)) return false;
    if (!pointer_niche && !bitptr_niche
        && presence_bit == UINT64_MAX) return false;
    core_list_t(ir_layout_bit_run_t) occupied = {0};
    for (size_t i = 0; i < child->n_occupied; i++)
        core_list_push(occupied, child->occupied[i]);
    if (!pointer_niche && !bitptr_niche) {
        core_list_push(occupied, ((ir_layout_bit_run_t){presence_bit, 1}));
        qsort(occupied.data, occupied.len, sizeof(*occupied.data),
              compare_layout_runs);
    }
    uint64_t intrinsic_bits = child->intrinsic_bits;
    if (!pointer_niche && !bitptr_niche
        && presence_bit == intrinsic_bits) intrinsic_bits++;
    ir_layout_access_t access = pointer_niche
        ? (ir_layout_access_t){.unit_width_bits = 64,
                               .value_width_bits = 64}
        : bitptr_niche
            ? (ir_layout_access_t){.unit_bit_offset = 64,
                                   .unit_width_bits = 64,
                                   .value_width_bits = 64}
            : (ir_layout_access_t){
                .unit_bit_offset = presence_bit & ~UINT64_C(7),
                .unit_width_bits = 8, .lsb = presence_bit % 8,
                .value_width_bits = 1,
            };
    ir_final_layout_t final = {
        .intrinsic_bits = intrinsic_bits,
        .alignment_bytes = child->alignment_bytes,
        .occupied = occupied.data, .n_occupied = occupied.len,
        .reserved = child->reserved, .n_reserved = child->n_reserved,
        .has_discriminant = true,
        .discriminant_is_niche = pointer_niche || bitptr_niche,
        .discriminant = access,
    };
    const ir_final_layout_t *existing = ir_layout_final(module, type,
                                                        target->triple);
    bool valid = existing ? same_option_layout(existing, &final)
        : intrinsic_bits <= UINT64_MAX - 7
            && ir_layout_pin_size(module, type, (intrinsic_bits + 7) / 8)
            && ir_layout_pin_align(module, type, child->alignment_bytes)
            && ir_layout_publish_final(module, type, target->triple, &final);
    core_list_free(occupied);
    *handled = valid;
    return valid;
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
    else if (th_kind_of(type) == TH_INDIRECTION) {
        th_type *referent = th_ind_referent(type);
        if (!referent || target->pointer_bits != 64
            || target->pointer_alignment_bytes != 8) return false;
        if (!th_is_closed(ir_module_types(module), type)) return true;
        const ir_final_layout_t *child = ir_layout_final(
            module, referent, target->triple);
        if (!child) {
            ir_machine_t child_machine = ir_machine_of(module, referent);
            if (child_machine.class == IR_CLASS_BOOL
                || child_machine.class == IR_CLASS_SINT
                || child_machine.class == IR_CLASS_UINT
                || child_machine.class == IR_CLASS_FLOAT) {
                bool child_handled = false;
                if (!publish_direct_or_alias(module, referent, target,
                        seen, &child_handled)) return false;
                child = ir_layout_final(module, referent, target->triple);
            }
        }
        if (!child) {
            th_type *resolved = th_resolve(referent);
            th_kind kind = th_kind_of(resolved);
            th_type *definition = kind == TH_NAMED
                ? ir_type_definition(module, resolved) : nullptr;
            if (kind != TH_RECORD && kind != TH_FUNCTION
                && (!definition || th_kind_of(definition) != TH_RECORD))
                return true;
        }
        const char *child_name = nullptr;
        th_named_info(th_resolve(referent), &child_name, nullptr,
                      nullptr, nullptr);
        bool packable = n00bb_abi_layout_packable(child)
            && (!child_name || strcmp(child_name, "atomic"));
        *handled = true;
        storage_bits = semantic_bits = packable ? 128 : 64;
        alignment_bytes = 8;
    }
    else if (th_kind_of(type) == TH_SEQUENCE) {
        *handled = true;
        if (target->pointer_bits != 64
            || target->pointer_alignment_bytes != 8) return false;
        storage_bits = semantic_bits = 128;
        alignment_bytes = 8;
    }
    else if (th_kind_of(type) == TH_RECORD)
        return publish_structural_record_layout(module, type, target,
                                                seen, handled);
    else if (th_kind_of(type) == TH_NAMED) {
        const char *name = nullptr;
        size_t arguments = 0;
        th_named_info(type, &name, nullptr, nullptr, &arguments);
        if (name && !strcmp(name, TH_TUPLE))
            return publish_tuple_layout(module, type, target, seen,
                                        handled);
        if (name && !strcmp(name, "option"))
            return publish_option_layout(module, type, target, seen,
                                         handled);
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
        if (th_kind_of(definition) == TH_RECORD) return true;
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

static int
compare_layout_runs(const void *left, const void *right)
{
    const ir_layout_bit_run_t *a = left;
    const ir_layout_bit_run_t *b = right;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    if (a->width != b->width) return a->width < b->width ? -1 : 1;
    return 0;
}

bool
n00bb_abi_publish_record_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    n00bb_record_policy_t policy, bool structural,
    const n00bb_abi_record_member_t *members, size_t count)
{
    if (!module || !type || !target || !target->triple
        || (count && !members)
        || strcmp(ir_module_target(module), target->triple)
        || ir_layout_final(module, type, target->triple)) return false;

    n00bb_record_field_t *fields = count
        ? core_alloc_array(n00bb_record_field_t, count) : nullptr;
    n00bb_record_placement_t *placements = count
        ? core_alloc_array(n00bb_record_placement_t, count) : nullptr;
    const ir_final_layout_t **children = count
        ? core_alloc_array(const ir_final_layout_t *, count) : nullptr;
    ir_final_member_layout_t *final_members = count
        ? core_alloc_array(ir_final_member_layout_t, count) : nullptr;
    bool ok = !count || (fields && placements && children && final_members);
    size_t occupied_count = 0, reserved_count = 0;
    for (size_t i = 0; ok && i < count; i++) {
        children[i] = ir_layout_final(module, members[i].type,
                                      target->triple);
        const ir_final_layout_t *child = children[i];
        bool packed = members[i].packed;
        if (!members[i].name || !members[i].type || !child
            || (policy == N00BB_RECORD_C
                && (!members[i].c_compatible || packed
                    || child->intrinsic_bits % 8))
            || (packed && (child->n_occupied != 1
                           || child->occupied[0].offset
                           || child->n_reserved
                           || !child->occupied[0].width))
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
            .extent_bits = packed ? child->occupied[0].width
                                  : child->intrinsic_bits,
            .occupied_bits = value_bits,
            .alignment_bits = packed ? 1
                                     : (uint64_t)child->alignment_bytes * 8,
            .c_compatible = members[i].c_compatible,
        };
    }

    n00bb_record_layout_t placement = {0};
    if (ok) ok = n00bb_abi_place_record(fields, count, policy,
                                       structural, placements, &placement);
    ir_layout_bit_run_t *occupied = ok && occupied_count
        ? core_alloc_array(ir_layout_bit_run_t, occupied_count) : nullptr;
    ir_layout_bit_run_t *member_occupied = ok && occupied_count
        ? core_alloc_array(ir_layout_bit_run_t, occupied_count) : nullptr;
    ir_layout_bit_run_t *reserved = ok && reserved_count
        ? core_alloc_array(ir_layout_bit_run_t, reserved_count) : nullptr;
    if (ok && ((occupied_count && (!occupied || !member_occupied))
               || (reserved_count && !reserved))) ok = false;

    size_t occupied_at = 0, reserved_at = 0;
    for (size_t i = 0; ok && i < count; i++) {
        const ir_final_layout_t *child = children[i];
        uint64_t base = placements[i].bit_offset;
        if (fields[i].extent_bits
            && (base > placement.intrinsic_bits
                || fields[i].extent_bits > placement.intrinsic_bits - base)) {
            ok = false;
            break;
        }
        final_members[i] = (ir_final_member_layout_t){
            .name = members[i].name,
            .type = members[i].type,
            .ordinal = members[i].ordinal,
            .bit_offset = base,
            .occupied = member_occupied
                ? member_occupied + occupied_at : nullptr,
            .n_occupied = child->n_occupied,
        };
        for (size_t j = 0; j < child->n_occupied; j++) {
            ir_layout_bit_run_t run = {
                .offset = base + child->occupied[j].offset,
                .width = child->occupied[j].width,
            };
            member_occupied[occupied_at] = run;
            occupied[occupied_at++] = run;
        }
        for (size_t j = 0; j < child->n_reserved; j++) {
            reserved[reserved_at++] = (ir_layout_bit_run_t){
                .offset = base + child->reserved[j].offset,
                .width = child->reserved[j].width,
            };
        }
    }
    if (ok && occupied_count)
        qsort(occupied, occupied_count, sizeof(*occupied),
              compare_layout_runs);
    if (ok && reserved_count)
        qsort(reserved, reserved_count, sizeof(*reserved),
              compare_layout_runs);
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
                                    placement.intrinsic_bits / 8
                                    + (placement.intrinsic_bits % 8 != 0))
        && ir_layout_pin_align(module, type, final.alignment_bytes);
    for (size_t i = 0; ok && i < count; i++)
        ok = ir_layout_pin_member(module, type, members[i].name,
            placements[i].bit_offset, fields[i].extent_bits);
    if (ok) ok = ir_layout_publish_final(module, type, target->triple, &final);

    core_free(reserved);
    core_free(member_occupied);
    core_free(occupied);
    core_free(final_members);
    core_free(children);
    core_free(placements);
    core_free(fields);
    return ok;
}

bool
n00bb_abi_publish_c_record_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    const n00bb_abi_record_member_t *members, size_t count)
{
    if (!count) return false;
    return n00bb_abi_publish_record_layout(module, type, target,
        N00BB_RECORD_C, false, members, count);
}

static bool
publish_tuple_layout(ir_module_t *module, th_type *type,
                     const core_target_desc_t *target, core_dict_t *seen,
                     bool *handled)
{
    size_t count = th_named_nargs(type);
    if (count > UINT32_MAX) return false;
    if (ir_layout_final(module, type, target->triple)) {
        *handled = true;
        return true;
    }
    n00bb_abi_record_member_t *members = count
        ? core_alloc_array(n00bb_abi_record_member_t, count) : nullptr;
    char **names = count ? core_alloc_array(char *, count) : nullptr;
    bool valid = !count || (members && names);
    bool ready = valid;
    for (size_t i = 0; valid && i < count; i++) {
        const th_arg *argument = th_named_arg(type, i);
        th_type *child = argument && argument->kind == TH_ARG_TYPE
            ? argument->type : nullptr;
        bool direct = false;
        if (!child || !publish_direct_or_alias(module, child, target,
                                               seen, &direct)) {
            valid = false;
            break;
        }
        if (!ir_layout_final(module, child, target->triple)) {
            ready = false;
            break;
        }
        size_t capacity = 3 * sizeof(size_t) + 2;
        names[i] = core_alloc_array(char, capacity);
        int written = names[i] ? snprintf(names[i], capacity, "%zu", i) : -1;
        if (written < 0 || (size_t)written >= capacity) {
            valid = false;
            break;
        }
        members[i] = (n00bb_abi_record_member_t){
            .name = names[i], .type = child, .ordinal = (uint32_t)i,
        };
    }
    if (valid && ready) {
        valid = n00bb_abi_publish_record_layout(module, type, target,
            N00BB_RECORD_ORDERED, false, members, count);
        *handled = valid;
    }
    for (size_t i = 0; i < count; i++) core_free(names ? names[i] : nullptr);
    core_free(names);
    core_free(members);
    return valid;
}

static bool
publish_structural_record_layout(ir_module_t *module, th_type *type,
                                 const core_target_desc_t *target,
                                 core_dict_t *seen, bool *handled)
{
    if (ir_layout_final(module, type, target->triple)) {
        *handled = true;
        return true;
    }
    size_t count = th_record_len(type);
    if (count > UINT32_MAX) return false;
    n00bb_abi_record_member_t *members = count
        ? core_alloc_array(n00bb_abi_record_member_t, count) : nullptr;
    bool ready = true;
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        const th_member *member = th_record_member(type, i);
        bool direct = false;
        if (!member || !member->name || !member->type
            || !publish_direct_or_alias(module, member->type, target,
                                        seen, &direct)) {
            valid = false;
            break;
        }
        const ir_final_layout_t *child = ir_layout_final(
            module, member->type, target->triple);
        if (!child) {
            ready = false;
            break;
        }
        bool packable = child->n_occupied == 1
            && child->occupied[0].offset == 0
            && child->occupied[0].width < child->intrinsic_bits
            && !child->n_reserved;
        members[i] = (n00bb_abi_record_member_t){
            .name = member->name,
            .type = member->type,
            .ordinal = (uint32_t)i,
            .packed = packable,
        };
    }
    if (valid && ready) {
        valid = n00bb_abi_publish_record_layout(module, type, target,
            N00BB_RECORD_DEFAULT, true, members, count);
        *handled = valid;
    }
    core_free(members);
    return valid;
}

static void
merge_bit_runs(core_list_t(ir_layout_bit_run_t) *runs)
{
    if (!runs->len) return;
    qsort(runs->data, runs->len, sizeof(*runs->data), compare_layout_runs);
    size_t out = 0;
    for (size_t i = 0; i < runs->len; i++) {
        ir_layout_bit_run_t run = runs->data[i];
        if (out && run.offset <= runs->data[out - 1].offset
                                + runs->data[out - 1].width) {
            uint64_t end = run.offset + run.width;
            uint64_t prior_end = runs->data[out - 1].offset
                + runs->data[out - 1].width;
            if (end > prior_end)
                runs->data[out - 1].width = end - runs->data[out - 1].offset;
        }
        else runs->data[out++] = run;
    }
    runs->len = out;
}

static bool
tag_in_gap(const core_target_desc_t *target, uint64_t start, uint64_t end,
           uint64_t payload_byte_bits, uint32_t tag_bits,
           uint64_t *bit, const core_target_scalar_t **unit)
{
    bool found = false;
    for (size_t i = 0; i < target->integer_storage_count; i++) {
        const core_target_scalar_t *storage = &target->integer_storage[i];
        if (storage->width_bits < tag_bits || !storage->alignment_bytes)
            continue;
        uint64_t alignment = (uint64_t)storage->alignment_bytes * 8;
        uint64_t candidate = start;
        uint64_t base = candidate / alignment * alignment;
        if (base > UINT64_MAX - storage->width_bits
            || candidate > UINT64_MAX - tag_bits
            || candidate + tag_bits > base + storage->width_bits) {
            if (base > UINT64_MAX - alignment) continue;
            candidate = base + alignment;
            base = candidate;
        }
        if (candidate > end || tag_bits > end - candidate
            || base > payload_byte_bits
            || storage->width_bits > payload_byte_bits - base) continue;
        if (!found || candidate < *bit
            || (candidate == *bit
                && storage->width_bits < (*unit)->width_bits)) {
            *bit = candidate;
            *unit = storage;
            found = true;
        }
    }
    return found;
}

static void
subtract_occupied(const core_list_t(ir_layout_bit_run_t) *reserved,
                  const core_list_t(ir_layout_bit_run_t) *occupied,
                  core_list_t(ir_layout_bit_run_t) *out)
{
    for (size_t i = 0; i < reserved->len; i++) {
        uint64_t at = reserved->data[i].offset;
        uint64_t end = at + reserved->data[i].width;
        for (size_t j = 0; j < occupied->len && at < end; j++) {
            uint64_t occupied_start = occupied->data[j].offset;
            uint64_t occupied_end = occupied_start + occupied->data[j].width;
            if (occupied_end <= at) continue;
            if (occupied_start >= end) break;
            if (occupied_start > at)
                core_list_push(*out, ((ir_layout_bit_run_t){
                    .offset = at, .width = occupied_start - at,
                }));
            if (occupied_end > at) at = occupied_end;
        }
        if (at < end)
            core_list_push(*out, ((ir_layout_bit_run_t){
                .offset = at, .width = end - at,
            }));
    }
}

bool
n00bb_abi_publish_variant_layout(
    ir_module_t *module, th_type *type, const core_target_desc_t *target,
    const n00bb_abi_variant_arm_t *arms, size_t count)
{
    if (!module || !type || !target || !target->triple || !arms || !count
        || count >= UINT32_MAX || !core_target_validate(target)
        || target->endian != CORE_TARGET_LITTLE_ENDIAN
        || strcmp(ir_module_target(module), target->triple)
        || ir_layout_final(module, type, target->triple)) return false;

    ir_final_member_layout_t *members = core_alloc_array(
        ir_final_member_layout_t, count);
    const ir_final_layout_t **children = core_alloc_array(
        const ir_final_layout_t *, count);
    if (!members || !children) {
        core_free(members);
        core_free(children);
        return false;
    }
    core_list_t(ir_layout_bit_run_t) occupied = {0};
    core_list_t(ir_layout_bit_run_t) blocked = {0};
    core_list_t(ir_layout_bit_run_t) child_reserved = {0};
    core_list_t(ir_layout_bit_run_t) reserved = {0};
    uint64_t payload_bits = 0;
    uint32_t alignment_bytes = 1;
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        children[i] = ir_layout_final(module, arms[i].type, target->triple);
        const ir_final_layout_t *child = children[i];
        if (!arms[i].name || !arms[i].type || arms[i].ordinal != i
            || !child || !child->alignment_bytes
            || child->intrinsic_bits > UINT64_MAX - 7) {
            valid = false;
            break;
        }
        if (child->intrinsic_bits > payload_bits)
            payload_bits = child->intrinsic_bits;
        if (child->alignment_bytes > alignment_bytes)
            alignment_bytes = child->alignment_bytes;
        members[i] = (ir_final_member_layout_t){
            .name = arms[i].name, .type = arms[i].type,
            .ordinal = arms[i].ordinal, .bit_offset = 0,
            .occupied = child->occupied, .n_occupied = child->n_occupied,
        };
        for (size_t j = 0; j < child->n_occupied; j++) {
            core_list_push(occupied, child->occupied[j]);
            core_list_push(blocked, child->occupied[j]);
        }
        for (size_t j = 0; j < child->n_reserved; j++) {
            core_list_push(child_reserved, child->reserved[j]);
            core_list_push(blocked, child->reserved[j]);
        }
    }
    if (valid && payload_bits > UINT64_MAX - 7) valid = false;
    uint64_t payload_byte_bits = valid
        ? (payload_bits + 7) & ~UINT64_C(7) : 0;
    if (valid) {
        merge_bit_runs(&blocked);
        merge_bit_runs(&occupied);
        merge_bit_runs(&child_reserved);
    }
    uint32_t tag_bits = 0;
    for (size_t n = count; n; n >>= 1) tag_bits++;
    uint64_t tag_bit = UINT64_MAX;
    const core_target_scalar_t *tag_unit = nullptr;
    for (size_t i = 0; valid && i <= blocked.len && !tag_unit; i++) {
        uint64_t gap_start = i ? blocked.data[i - 1].offset
            + blocked.data[i - 1].width : 0;
        uint64_t gap_end = i < blocked.len
            ? blocked.data[i].offset : payload_bits;
        if (gap_start < gap_end)
            tag_in_gap(target, gap_start, gap_end, payload_byte_bits,
                       tag_bits, &tag_bit, &tag_unit);
    }
    uint64_t unit_bit = 0;
    if (valid && !tag_unit) {
        tag_unit = core_target_integer_for_bits(target, tag_bits);
        if (!tag_unit || !tag_unit->alignment_bytes)
            valid = false;
        else {
            uint64_t alignment = (uint64_t)tag_unit->alignment_bytes * 8;
            uint64_t remainder = payload_bits % alignment;
            uint64_t padding = remainder ? alignment - remainder : 0;
            if (payload_bits > UINT64_MAX - padding - tag_bits)
                valid = false;
            else tag_bit = payload_bits + padding;
        }
    }
    if (valid) {
        uint64_t alignment = (uint64_t)tag_unit->alignment_bytes * 8;
        unit_bit = tag_bit / alignment * alignment;
        if (tag_unit->alignment_bytes > alignment_bytes)
            alignment_bytes = tag_unit->alignment_bytes;
        core_list_push(occupied, ((ir_layout_bit_run_t){
            .offset = tag_bit, .width = tag_bits,
        }));
        merge_bit_runs(&occupied);
        subtract_occupied(&child_reserved, &occupied, &reserved);
        merge_bit_runs(&reserved);
    }
    uint64_t intrinsic_bits = valid && tag_bit + tag_bits > payload_bits
        ? tag_bit + tag_bits : payload_bits;
    ir_final_layout_t final = {
        .intrinsic_bits = intrinsic_bits,
        .alignment_bytes = alignment_bytes,
        .occupied = occupied.data, .n_occupied = occupied.len,
        .reserved = reserved.data, .n_reserved = reserved.len,
        .members = members, .n_members = count,
        .member_overlap_region = {.offset = 0, .width = payload_bits},
        .has_discriminant = true,
        .discriminant = {
            .unit_bit_offset = unit_bit,
            .unit_width_bits = tag_unit ? tag_unit->width_bits : 0,
            .lsb = (uint32_t)(tag_bit - unit_bit),
            .value_width_bits = tag_bits,
        },
    };
    if (valid) valid = ir_layout_pin_size(module, type,
            intrinsic_bits / 8 + (intrinsic_bits % 8 != 0))
        && ir_layout_pin_align(module, type, alignment_bytes)
        && ir_layout_pin_variant(module, type, (ir_variant_repr_t){
            .tag_bit_offset = {.value = tag_bit},
            .tag_bit_width = {.value = tag_bits},
        });
    for (size_t i = 0; valid && i < count; i++)
        valid = ir_layout_pin_member(module, type, arms[i].name, 0,
                                     children[i]->intrinsic_bits);
    if (valid)
        valid = ir_layout_publish_final(module, type, target->triple, &final);
    core_list_free(occupied);
    core_list_free(blocked);
    core_list_free(child_reserved);
    core_list_free(reserved);
    core_free(children);
    core_free(members);
    return valid;
}
