#include "lib/target.h"

#include <string.h>

static const core_target_scalar_t initial_integer_storage[] = {
    {8, 1}, {16, 2}, {32, 4}, {64, 8}, {128, 16},
};

static const core_target_scalar_t initial_floating_storage[] = {
    {32, 4}, {64, 8},
};

static const core_target_atomic_t initial_atomic_storage[] = {
    {8, 1, 0}, {16, 2, 0}, {32, 4, 0}, {64, 8, 0},
    {128, 16, CORE_TARGET_FEATURE_NATIVE_ATOMIC128},
};

bool core_target_profile(core_string_t triple, uint64_t enabled_features,
                         core_target_desc_t *out)
{
    if (!triple.data || !out
        || (enabled_features & ~CORE_TARGET_FEATURE_NATIVE_ATOMIC128))
        return false;

    core_target_desc_t profile = {
        .endian = CORE_TARGET_LITTLE_ENDIAN,
        .pointer_bits = 64,
        .pointer_alignment_bytes = 8,
        .alu_bits = 64,
        .stack_alignment_bytes = 16,
        .enabled_features = enabled_features,
        .integer_storage = initial_integer_storage,
        .integer_storage_count = sizeof(initial_integer_storage)
                                 / sizeof(initial_integer_storage[0]),
        .floating_storage = initial_floating_storage,
        .floating_storage_count = sizeof(initial_floating_storage)
                                  / sizeof(initial_floating_storage[0]),
        .atomic_storage = initial_atomic_storage,
        .atomic_storage_count = sizeof(initial_atomic_storage)
                                / sizeof(initial_atomic_storage[0]),
    };
    if (core_string_eq(triple, CORE_STRING_STATIC("x86_64-linux"))) {
        profile.triple = CORE_STRING_STATIC("x86_64-linux");
        profile.object_format = CORE_TARGET_OBJECT_ELF;
        profile.call_abi = CORE_TARGET_CALL_SYSV_AMD64;
    }
    else if (core_string_eq(triple, CORE_STRING_STATIC("arm64-macos"))) {
        profile.triple = CORE_STRING_STATIC("arm64-macos");
        profile.object_format = CORE_TARGET_OBJECT_MACH_O;
        profile.call_abi = CORE_TARGET_CALL_DARWIN_AARCH64;
    }
    else if (core_string_eq(triple, CORE_STRING_STATIC("arm64-linux"))) {
        profile.triple = CORE_STRING_STATIC("arm64-linux");
        profile.object_format = CORE_TARGET_OBJECT_ELF;
        profile.call_abi = CORE_TARGET_CALL_AAPCS64;
    }
    else if (core_string_eq(triple, CORE_STRING_STATIC("x86_64-windows"))) {
        profile.triple = CORE_STRING_STATIC("x86_64-windows");
        profile.object_format = CORE_TARGET_OBJECT_COFF;
        profile.call_abi = CORE_TARGET_CALL_WINDOWS_X64;
    }
    else return false;

    if (!core_target_validate(&profile)) return false;
    *out = profile;
    return true;
}

bool core_target_host(core_target_desc_t *out)
{
#if defined(__APPLE__) && defined(__aarch64__)
    return core_target_profile(CORE_STRING_STATIC("arm64-macos"), 0, out);
#elif defined(__linux__) && defined(__x86_64__)
    return core_target_profile(CORE_STRING_STATIC("x86_64-linux"), 0, out);
#elif defined(__linux__) && defined(__aarch64__)
    return core_target_profile(CORE_STRING_STATIC("arm64-linux"), 0, out);
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return core_target_profile(CORE_STRING_STATIC("x86_64-windows"), 0, out);
#else
    (void)out;
    return false;
#endif
}

static bool power_of_two(uint32_t value)
{
    return value && !(value & (value - 1));
}

bool core_target_validate(const core_target_desc_t *target)
{
    if (!target || !target->triple.data || !target->triple.u8_bytes
        || target->endian > CORE_TARGET_BIG_ENDIAN
        || target->object_format > CORE_TARGET_OBJECT_OTHER
        || target->call_abi > CORE_TARGET_CALL_OTHER
        || !target->pointer_bits || target->pointer_bits % 8
        || !target->alu_bits || target->alu_bits % 8
        || !power_of_two(target->pointer_alignment_bytes)
        || !power_of_two(target->stack_alignment_bytes)
        || !target->integer_storage_count || !target->integer_storage
        || (target->floating_storage_count && !target->floating_storage)
        || (target->atomic_storage_count && !target->atomic_storage))
        return false;

    uint32_t previous_width = 0;
    for (size_t i = 0; i < target->integer_storage_count; i++) {
        core_target_scalar_t entry = target->integer_storage[i];
        if (entry.width_bits <= previous_width || entry.width_bits % 8
            || !power_of_two(entry.alignment_bytes))
            return false;
        previous_width = entry.width_bits;
    }

    previous_width = 0;
    for (size_t i = 0; i < target->floating_storage_count; i++) {
        core_target_scalar_t entry = target->floating_storage[i];
        if (entry.width_bits <= previous_width || entry.width_bits % 8
            || !power_of_two(entry.alignment_bytes))
            return false;
        previous_width = entry.width_bits;
    }

    previous_width = 0;
    for (size_t i = 0; i < target->atomic_storage_count; i++) {
        core_target_atomic_t entry = target->atomic_storage[i];
        if (entry.width_bits < previous_width || !entry.width_bits
            || entry.width_bits % 8
            || !power_of_two(entry.alignment_bytes))
            return false;
        previous_width = entry.width_bits;
    }
    return true;
}

const core_target_scalar_t *core_target_integer_for_bits(
    const core_target_desc_t *target, uint32_t minimum_bits)
{
    if (!target || !minimum_bits || !target->integer_storage)
        return nullptr;
    for (size_t i = 0; i < target->integer_storage_count; i++)
        if (target->integer_storage[i].width_bits >= minimum_bits)
            return &target->integer_storage[i];
    return nullptr;
}

const core_target_scalar_t *core_target_float_storage(
    const core_target_desc_t *target, uint32_t width_bits)
{
    if (!target || !width_bits || !target->floating_storage)
        return nullptr;
    for (size_t i = 0; i < target->floating_storage_count; i++)
        if (target->floating_storage[i].width_bits == width_bits)
            return &target->floating_storage[i];
    return nullptr;
}

const core_target_atomic_t *core_target_atomic_for_bits(
    const core_target_desc_t *target, uint32_t minimum_bits)
{
    if (!target || !minimum_bits || !target->atomic_storage)
        return nullptr;
    for (size_t i = 0; i < target->atomic_storage_count; i++) {
        const core_target_atomic_t *entry = &target->atomic_storage[i];
        if (entry->width_bits >= minimum_bits
            && (entry->required_features & target->enabled_features)
                == entry->required_features)
            return entry;
    }
    return nullptr;
}
