#include "lib/target.h"
#include "harness.h"

#include <string.h>

static const core_target_scalar_t integers[] = {
    {8, 1}, {16, 2}, {32, 4}, {64, 8}, {128, 16},
};

static const core_target_scalar_t floats[] = {
    {32, 4}, {64, 8},
};

static const core_target_atomic_t atomics[] = {
    {8, 1, 0}, {16, 2, 0}, {32, 4, 0}, {64, 8, 0},
    {128, 16, UINT64_C(1)},
};

static const core_target_atomic_t alternate_atomics[] = {
    {64, 8, 0}, {128, 16, UINT64_C(1)}, {128, 16, UINT64_C(2)},
};

int main(void)
{
    core_target_desc_t target = {
        .triple = "test-64",
        .endian = CORE_TARGET_LITTLE_ENDIAN,
        .object_format = CORE_TARGET_OBJECT_ELF,
        .call_abi = CORE_TARGET_CALL_OTHER,
        .pointer_bits = 64,
        .pointer_alignment_bytes = 8,
        .alu_bits = 64,
        .stack_alignment_bytes = 16,
        .integer_storage = integers,
        .integer_storage_count = sizeof(integers) / sizeof(integers[0]),
        .floating_storage = floats,
        .floating_storage_count = sizeof(floats) / sizeof(floats[0]),
        .atomic_storage = atomics,
        .atomic_storage_count = sizeof(atomics) / sizeof(atomics[0]),
    };

    check("valid target descriptor", core_target_validate(&target));
    const core_target_scalar_t *integer =
        core_target_integer_for_bits(&target, 65);
    check("smallest sufficient integer storage",
          integer && integer->width_bits == 128
              && integer->alignment_bytes == 16);
    check("unsupported integer width is absent",
          !core_target_integer_for_bits(&target, 129)
              && !core_target_integer_for_bits(&target, 0));
    const core_target_scalar_t *floating =
        core_target_float_storage(&target, 64);
    check("exact floating storage width",
          floating && floating->alignment_bytes == 8
              && !core_target_float_storage(&target, 65));
    check("unavailable atomic feature excludes 128 bits",
          !core_target_atomic_for_bits(&target, 65));
    target.enabled_features = UINT64_C(1);
    const core_target_atomic_t *atomic =
        core_target_atomic_for_bits(&target, 65);
    check("enabled native atomic width is selected",
          atomic && atomic->width_bits == 128);

    target.atomic_storage = alternate_atomics;
    target.atomic_storage_count = sizeof(alternate_atomics)
                                  / sizeof(alternate_atomics[0]);
    target.enabled_features = UINT64_C(2);
    atomic = core_target_atomic_for_bits(&target, 65);
    check("alternate features may supply the same native width",
          core_target_validate(&target) && atomic
              && atomic->required_features == UINT64_C(2));

    core_target_scalar_t unordered[] = {{16, 2}, {8, 1}};
    target.integer_storage = unordered;
    target.integer_storage_count = 2;
    check("unordered widths are rejected", !core_target_validate(&target));
    target.integer_storage = integers;
    target.integer_storage_count = sizeof(integers) / sizeof(integers[0]);
    core_target_scalar_t unordered_floats[] = {{64, 8}, {32, 4}};
    target.floating_storage = unordered_floats;
    check("unordered floating widths are rejected",
          !core_target_validate(&target));
    target.floating_storage = floats;
    target.stack_alignment_bytes = 3;
    check("invalid stack alignment is rejected",
          !core_target_validate(&target));

    static const struct {
        const char *triple;
        core_target_object_format_t object_format;
        core_target_call_abi_t call_abi;
    } profiles[] = {
        {"x86_64-linux", CORE_TARGET_OBJECT_ELF,
         CORE_TARGET_CALL_SYSV_AMD64},
        {"arm64-macos", CORE_TARGET_OBJECT_MACH_O,
         CORE_TARGET_CALL_DARWIN_AARCH64},
        {"x86_64-windows", CORE_TARGET_OBJECT_COFF,
         CORE_TARGET_CALL_WINDOWS_X64},
    };
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        core_target_desc_t profile = {0};
        check("known profile", core_target_profile(profiles[i].triple, 0,
                                                     &profile));
        check("profile identity and native convention",
              !strcmp(profile.triple, profiles[i].triple)
                  && profile.endian == CORE_TARGET_LITTLE_ENDIAN
                  && profile.object_format == profiles[i].object_format
                  && profile.call_abi == profiles[i].call_abi
                  && profile.pointer_bits == 64
                  && profile.pointer_alignment_bytes == 8
                  && profile.alu_bits == 64
                  && profile.stack_alignment_bytes == 16);
        const core_target_scalar_t *wide =
            core_target_integer_for_bits(&profile, 128);
        check("profile 128-bit integer storage",
              wide && wide->width_bits == 128
                  && wide->alignment_bytes == 16);
        const core_target_scalar_t *f64 =
            core_target_float_storage(&profile, 64);
        check("profile f64 storage", f64 && f64->alignment_bytes == 8);
        check("baseline excludes 128-bit atomics",
              !core_target_atomic_for_bits(&profile, 128));
        check("profile accepts native atomic guarantee",
              core_target_profile(profiles[i].triple,
                  CORE_TARGET_FEATURE_NATIVE_ATOMIC128, &profile)
                  && core_target_atomic_for_bits(&profile, 128));
    }
    core_target_desc_t untouched = {.triple = "unchanged"};
    check("unknown profile leaves output untouched",
          !core_target_profile("unknown", 0, &untouched)
              && !strcmp(untouched.triple, "unchanged"));
    check("unknown feature leaves output untouched",
          !core_target_profile("x86_64-linux", UINT64_C(1) << 63,
                               &untouched)
              && !strcmp(untouched.triple, "unchanged"));
    return report("lib-target");
}
