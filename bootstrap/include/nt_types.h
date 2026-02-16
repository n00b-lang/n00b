/**
 * @file nt_types.h
 * @brief Non-terminal type definitions for fast grammar node comparisons.
 *
 * This file defines an enum for all grammar non-terminals and provides
 * bitfield-based set operations for efficient node type checking.
 *
 * Instead of strcmp() chains like:
 *   if (node_name_is(n, "declarator") || node_name_is(n, "direct_declarator"))
 *
 * Use bitfield checks:
 *   if (NT_IN_SET(n->nt_id, NT_SET_DECLARATORS))
 */
#pragma once

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/**
 * X-macro list of all grammar non-terminals.
 * Auto-generated from parse.c and parse_support.c at build time.
 * The enum and lookup table are both generated from this list.
 *
 * To regenerate: meson compile -C build
 * Source: scripts/gen_nt_list.sh
 */
#include "nt_list.inc"

/**
 * Enum of all non-terminal types.
 * NT_NONE (0) represents unknown/invalid.
 */
typedef enum {
    NT_NONE = 0,
#define X_ENUM(name) NT_##name,
    NT_LIST(X_ENUM)
#undef X_ENUM
    NT_COUNT
} nt_type_t;

_Static_assert(NT_COUNT <= 128, "Too many NTs for __uint128_t bitfield");

/**
 * Bitfield type for NT sets (supports up to 128 NTs).
 */
typedef __uint128_t nt_set_t;

/** Create a bitfield with a single NT set. */
#define NT_BIT(nt) ((nt_set_t)1 << (nt))

/** Check if an NT is in a set. */
#define NT_IN_SET(nt_id, set) (NT_BIT(nt_id) & (set))

/* ============================================================================
 * Common NT Sets - used for multi-way comparisons in the codebase
 * ============================================================================ */

/** Declarator-related nodes (for extract_declarator_name, etc.) */
#define NT_SET_DECLARATORS \
    (NT_BIT(NT_declarator) | NT_BIT(NT_direct_declarator) | NT_BIT(NT_init_declarator) | NT_BIT(NT_array_declarator) | NT_BIT(NT_function_declarator))

/** Tag specifiers (struct/union/enum) */
#define NT_SET_TAG_SPECIFIERS \
    (NT_BIT(NT_struct_or_union_specifier) | NT_BIT(NT_enum_specifier))

/** Type specifier nodes */
#define NT_SET_TYPE_SPECIFIERS \
    (NT_BIT(NT_type_specifier) | NT_BIT(NT_declaration_specifier))

/**
 * List structures that should be flattened.
 * These are true lists (comma-separated or sequential items), NOT binary
 * expression chains like additive_expression which have specific associativity.
 */
#define NT_SET_FLATTENABLE_LISTS \
    (NT_BIT(NT_argument_expression_list) | NT_BIT(NT_attribute_list) | NT_BIT(NT_attribute_specifier_sequence) | NT_BIT(NT_balanced_token_sequence) | NT_BIT(NT_block_item_list) | NT_BIT(NT_declaration_specifiers) | NT_BIT(NT_designator_list) | NT_BIT(NT_enumerator_list) | NT_BIT(NT_generic_assoc_list) | NT_BIT(NT_init_declarator_list) | NT_BIT(NT_keyword_param_list) | NT_BIT(NT_member_declaration_list) | NT_BIT(NT_member_declarator_list) | NT_BIT(NT_parameter_list) | NT_BIT(NT_specifier_qualifier_list) | NT_BIT(NT_storage_class_specifiers) | NT_BIT(NT_translation_unit) | NT_BIT(NT_type_qualifier_list))

/* ============================================================================
 * NT Lookup - map function name string to nt_type_t
 * ============================================================================ */

/**
 * Lookup table entry for NT name -> ID mapping.
 */
typedef struct {
    const char *name;
    nt_type_t   id;
} nt_lookup_entry_t;

/**
 * Sorted lookup table (generated from NT_LIST).
 */
static const nt_lookup_entry_t nt_lookup_table[] = {
#define X_LOOKUP(name) {#name, NT_##name},
    NT_LIST(X_LOOKUP)
#undef X_LOOKUP
};

#define NT_LOOKUP_SIZE (sizeof(nt_lookup_table) / sizeof(nt_lookup_table[0]))

/**
 * Look up NT ID from function name (e.g., "declarator_0" -> NT_declarator).
 * Uses binary search - O(log n) with ~7 comparisons for 108 entries.
 *
 * @param name  Function name (may include branch suffix like "_0")
 * @return NT ID, or NT_NONE if not found
 */
static inline nt_type_t
nt_lookup(const char *name)
{
    if (!name) {
        return NT_NONE;
    }

    // Calculate base name length (strip "_N" or "_NN" branch suffix)
    size_t len = strlen(name);
    if (len >= 3 && name[len - 3] == '_' && isdigit((unsigned char)name[len - 2]) && isdigit((unsigned char)name[len - 1])) {
        // Two-digit suffix like "_10"
        len -= 3;
    }
    else if (len >= 2 && name[len - 2] == '_' && isdigit((unsigned char)name[len - 1])) {
        // Single-digit suffix like "_0"
        len -= 2;
    }

    // Binary search
    int lo = 0;
    int hi = (int)NT_LOOKUP_SIZE - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strncmp(name, nt_lookup_table[mid].name, len);

        if (cmp == 0) {
            // Check for exact match (table entry same length as our base)
            if (nt_lookup_table[mid].name[len] == '\0') {
                return nt_lookup_table[mid].id;
            }
            // Our name is a prefix of table entry - go lower
            cmp = -1;
        }

        if (cmp < 0) {
            hi = mid - 1;
        }
        else {
            lo = mid + 1;
        }
    }

    return NT_NONE;
}
