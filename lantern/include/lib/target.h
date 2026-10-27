#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"

typedef enum {
    CORE_TARGET_LITTLE_ENDIAN,
    CORE_TARGET_BIG_ENDIAN,
} core_target_endian_t;

typedef enum {
    CORE_TARGET_OBJECT_ELF,
    CORE_TARGET_OBJECT_MACH_O,
    CORE_TARGET_OBJECT_COFF,
    CORE_TARGET_OBJECT_OTHER,
} core_target_object_format_t;

enum {
    CORE_OBJECT_MACHINE_ELF_X86_64 = 62,
    CORE_OBJECT_MACHINE_ELF_AARCH64 = 183,
    CORE_OBJECT_MACHINE_MACHO_X86_64 = 0x01000007,
    CORE_OBJECT_MACHINE_MACHO_ARM64 = 0x0100000c,
    CORE_OBJECT_MACHINE_COFF_X86_64 = 0x8664,
    CORE_OBJECT_MACHINE_COFF_ARM64 = 0xaa64,
};

typedef enum {
    CORE_TARGET_CALL_SYSV_AMD64,
    CORE_TARGET_CALL_DARWIN_AARCH64,
    CORE_TARGET_CALL_WINDOWS_X64,
    CORE_TARGET_CALL_AAPCS64,
    CORE_TARGET_CALL_OTHER,
} core_target_call_abi_t;

typedef struct {
    uint32_t width_bits;
    uint32_t alignment_bytes;
} core_target_scalar_t;

typedef struct {
    uint32_t width_bits;
    uint32_t alignment_bytes;
    uint64_t required_features;
} core_target_atomic_t;

typedef struct {
    core_string_t triple;
    core_target_endian_t endian;
    core_target_object_format_t object_format;
    core_target_call_abi_t call_abi;
    uint32_t pointer_bits;
    uint32_t pointer_alignment_bytes;
    uint32_t alu_bits;
    uint32_t stack_alignment_bytes;
    uint64_t enabled_features;
    const core_target_scalar_t *integer_storage;
    size_t integer_storage_count;
    const core_target_scalar_t *floating_storage;
    size_t floating_storage_count;
    const core_target_atomic_t *atomic_storage;
    size_t atomic_storage_count;
} core_target_desc_t;

#define CORE_TARGET_FEATURE_NATIVE_ATOMIC128 (UINT64_C(1) << 0)

bool core_target_profile(core_string_t triple, uint64_t enabled_features,
                         core_target_desc_t *out);
bool core_target_host(core_target_desc_t *out);
bool core_target_validate(const core_target_desc_t *target);
const core_target_scalar_t *core_target_integer_for_bits(
    const core_target_desc_t *target, uint32_t minimum_bits);
const core_target_scalar_t *core_target_float_storage(
    const core_target_desc_t *target, uint32_t width_bits);
const core_target_atomic_t *core_target_atomic_for_bits(
    const core_target_desc_t *target, uint32_t minimum_bits);
