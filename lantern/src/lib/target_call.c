#include "lib/target_call.h"

#include <limits.h>

enum { TARGET_WORD_BITS = 64, TARGET_WORD_BYTES = TARGET_WORD_BITS / CHAR_BIT };
_Static_assert(CHAR_BIT == 8, "supported call ABIs use eight-bit bytes");

static bool
power_of_two(uint32_t value)
{
    return value && !(value & (value - 1));
}

static bool
valid_aggregate(const core_target_desc_t *target,
                const core_target_aggregate_t *aggregate)
{
    if (!target || !aggregate || !aggregate->alignment_bytes
        || !power_of_two(aggregate->alignment_bytes)
        || aggregate->size_bytes > UINT64_MAX / CHAR_BIT
        || (aggregate->atom_count && !aggregate->atoms)
        || (aggregate->size_bytes && !aggregate->atom_count)) return false;
    uint64_t size_bits = aggregate->size_bytes * CHAR_BIT;
    for (size_t i = 0; i < aggregate->atom_count; i++) {
        const core_target_call_atom_t *atom = &aggregate->atoms[i];
        if (!atom->width_bits || !power_of_two(atom->alignment_bits)
            || atom->width_bits > size_bits
            || atom->offset_bits > size_bits - atom->width_bits
            || atom->kind < CORE_TARGET_ATOM_INTEGER
            || atom->kind > CORE_TARGET_ATOM_FLOAT)
            return false;
        if (atom->kind == CORE_TARGET_ATOM_FLOAT) {
            if (atom->width_bits > TARGET_WORD_BITS) return false;
            const core_target_scalar_t *floating =
                core_target_float_storage(target, (uint32_t)atom->width_bits);
            if (!floating || atom->alignment_bits
                != (uint64_t)floating->alignment_bytes * CHAR_BIT)
                return false;
        }
    }
    return true;
}

static core_target_transport_t
memory_transport(bool is_return, core_target_call_abi_t abi)
{
    return is_return || abi != CORE_TARGET_CALL_SYSV_AMD64
        ? CORE_TARGET_TRANSPORT_INDIRECT : CORE_TARGET_TRANSPORT_STACK;
}

static bool
aarch64_hfa(const core_target_desc_t *target,
           const core_target_aggregate_t *aggregate,
           core_target_aggregate_class_t *result)
{
    if (!aggregate->atom_count || aggregate->atom_count > 4) return false;
    uint64_t width = aggregate->atoms[0].width_bits;
    const core_target_scalar_t *floating =
        core_target_float_storage(target, (uint32_t)width);
    if (!floating || width > UINT64_MAX / aggregate->atom_count
        || width * aggregate->atom_count != aggregate->size_bytes * CHAR_BIT
        || aggregate->alignment_bytes != floating->alignment_bytes)
        return false;
    bool seen[4] = {0};
    for (size_t i = 0; i < aggregate->atom_count; i++) {
        const core_target_call_atom_t *atom = &aggregate->atoms[i];
        if (atom->kind != CORE_TARGET_ATOM_FLOAT
            || atom->width_bits != width
            || atom->alignment_bits != width
            || atom->offset_bits % width) return false;
        uint64_t ordinal = atom->offset_bits / width;
        if (ordinal >= aggregate->atom_count || seen[ordinal]) return false;
        seen[ordinal] = true;
    }
    result->transport = CORE_TARGET_TRANSPORT_DIRECT;
    result->lane_count = (uint8_t)aggregate->atom_count;
    for (size_t i = 0; i < aggregate->atom_count; i++)
        result->lanes[i] = CORE_TARGET_LANE_FLOAT;
    return true;
}

bool
core_target_classify_aggregate(const core_target_desc_t *target,
                               const core_target_aggregate_t *aggregate,
                               bool force_indirect, bool is_return,
                               core_target_aggregate_class_t *out)
{
    if (!out || !valid_aggregate(target, aggregate)
        || target->call_abi < CORE_TARGET_CALL_SYSV_AMD64
        || target->call_abi > CORE_TARGET_CALL_AAPCS64)
        return false;
    core_target_aggregate_class_t result = {0};
    if (!aggregate->size_bytes) {
        result.transport = CORE_TARGET_TRANSPORT_IGNORE;
        *out = result;
        return true;
    }
    if (force_indirect) {
        *out = (core_target_aggregate_class_t){
            .transport = CORE_TARGET_TRANSPORT_INDIRECT};
        return true;
    }
    switch (target->call_abi) {
    case CORE_TARGET_CALL_SYSV_AMD64:
        if (aggregate->size_bytes > 2 * TARGET_WORD_BYTES) {
            result.transport = memory_transport(is_return, target->call_abi);
            break;
        }
        result.transport = CORE_TARGET_TRANSPORT_DIRECT;
        result.lane_count = (uint8_t)((aggregate->size_bytes
                                      + TARGET_WORD_BYTES - 1)
                                     / TARGET_WORD_BYTES);
        for (size_t i = 0; i < aggregate->atom_count; i++) {
            const core_target_call_atom_t *atom = &aggregate->atoms[i];
            if (atom->offset_bits % atom->alignment_bits
                || atom->alignment_bits >
                    (uint64_t)aggregate->alignment_bytes * CHAR_BIT) {
                result = (core_target_aggregate_class_t){
                    .transport = memory_transport(is_return, target->call_abi)};
                break;
            }
            size_t first = (size_t)(atom->offset_bits / TARGET_WORD_BITS);
            size_t last = (size_t)((atom->offset_bits
                                   + atom->width_bits - 1)
                                  / TARGET_WORD_BITS);
            for (size_t lane = first; lane <= last; lane++) {
                core_target_lane_t incoming = atom->kind
                    == CORE_TARGET_ATOM_FLOAT
                    ? CORE_TARGET_LANE_FLOAT : CORE_TARGET_LANE_INTEGER;
                if (result.lanes[lane] == CORE_TARGET_LANE_NONE
                    || incoming == CORE_TARGET_LANE_INTEGER)
                    result.lanes[lane] = incoming;
            }
        }
        break;
    case CORE_TARGET_CALL_DARWIN_AARCH64:
    case CORE_TARGET_CALL_AAPCS64:
        if (aarch64_hfa(target, aggregate, &result)) break;
        if (aggregate->size_bytes > 2 * TARGET_WORD_BYTES) {
            result.transport = CORE_TARGET_TRANSPORT_INDIRECT;
            break;
        }
        result.transport = CORE_TARGET_TRANSPORT_DIRECT;
        result.lane_count = (uint8_t)((aggregate->size_bytes
                                      + TARGET_WORD_BYTES - 1)
                                     / TARGET_WORD_BYTES);
        for (size_t i = 0; i < result.lane_count; i++)
            result.lanes[i] = CORE_TARGET_LANE_INTEGER;
        break;
    case CORE_TARGET_CALL_WINDOWS_X64:
        if (aggregate->size_bytes == 1 || aggregate->size_bytes == 2
            || aggregate->size_bytes == 4 || aggregate->size_bytes == 8) {
            result.transport = CORE_TARGET_TRANSPORT_DIRECT;
            result.lane_count = 1;
            result.lanes[0] = CORE_TARGET_LANE_INTEGER;
        }
        else result.transport = CORE_TARGET_TRANSPORT_INDIRECT;
        break;
    default:
        return false;
    }
    *out = result;
    return true;
}
