/**
 * @file hash.h
 * @brief Runtime type hashing: n00b type-checker types → typehash values.
 *
 * Converts `n00b_tc_type_t *` to the same `uint64_t` hash that the
 * compile-time `typehash(T)` ncc transform produces for the corresponding
 * C type.  This allows runtime-boxed `n00b_any_t` values to be looked up
 * in the type registry.
 *
 * ## Mapping (n00b → C)
 *
 * | n00b type       | C type            |
 * |-----------------|-------------------|
 * | `int`, `i64`    | `int64_t`         |
 * | `i8`            | `int8_t`          |
 * | `i16`           | `int16_t`         |
 * | `i32`           | `int32_t`         |
 * | `u8`            | `uint8_t`         |
 * | `u16`           | `uint16_t`        |
 * | `u32`           | `uint32_t`        |
 * | `u64`           | `uint64_t`        |
 * | `f32`           | `float`           |
 * | `f64`           | `double`          |
 * | `bool`          | `bool`            |
 * | `string`        | `n00b_string_t`   |
 * | `nil`, `void`   | `void`            |
 * | `list[T]`       | `n00b_list_t`     |
 * | `dict[K, V]`    | `n00b_dict_t`     |
 * | `result[T]`     | `n00b_result_t`   |
 * | `ref[T]`        | pointer (PTR)     |
 * | function types  | function pointer  |
 * | unresolved Var  | `n00b_any_t`      |
 */
#pragma once

#include "typecheck/types.h"

/**
 * @brief Compute the typehash for an n00b type-checker type.
 *
 * Resolves the type (chasing union-find links), maps to the
 * corresponding C type name, and returns `SHA256(c_name)` truncated
 * to 64 bits — identical to what `typehash(T)` produces at compile time.
 *
 * @param t  The type to hash (may be a forwarded/unified type).
 * @return   The 64-bit typehash, or 0 for unresolvable types.
 */
uint64_t n00b_tc_type_to_hash(n00b_tc_type_t *t);

/**
 * @brief Hash a raw C type name string (like ncc's `n00b_type_hash_u64`).
 *
 * SHA256 of the string, first 8 bytes as big-endian uint64.
 */
uint64_t n00b_type_hash_cname(const char *cname);
