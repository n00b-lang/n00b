#pragma once
/** @file rich_desc.h
 *  @brief Parse rich markup descriptors into segment arrays.
 *
 *  A rich markup descriptor is a string containing literal text
 *  interspersed with tags delimited by `[|...|]` or `«...»`.
 *
 *  ### Supported tag forms
 *
 *  | Syntax               | Meaning                         |
 *  |----------------------|---------------------------------|
 *  | `[|b|]`              | Turn on property (bold)         |
 *  | `[|/b|]`             | Turn off property               |
 *  | `[|name|]`           | Push named style from registry  |
 *  | `[|/name|]`          | Pop named style                 |
 *  | `[|@role|]`          | Push text role                  |
 *  | `[|/@role|]`         | Pop text role                   |
 *  | `[|/|]`              | Reset all styles                |
 *  | `[|#|]`              | Auto-indexed substitution       |
 *  | `[|#N|]`             | Explicit-indexed substitution   |
 *  | `[|#N:spec|]`        | Substitution with format spec   |
 *  | `[|#!|]`             | Substitution, strip styling     |
 *  | `«tag»` ... `«/tag»` | Guillemet shorthand             |
 *  | `\[` / `\\` / `\«`  | Escape next character           |
 *
 *  Parsed results are cached by XXH3 hash of the descriptor bytes.
 *
 *  ### Related modules
 *
 *  - `strings/style_registry.h` -- named style / role lookup
 *  - `strings/format_spec.h` -- substitution format specifiers
 */

#include "n00b.h"
#include "core/alloc.h"

// ===================================================================
// Segment kinds
// ===================================================================

typedef enum {
    N00B_RICH_TEXT,       /**< Literal text span */
    N00B_RICH_STYLE_ON,  /**< Push named style */
    N00B_RICH_STYLE_OFF, /**< Pop named style */
    N00B_RICH_PROP_ON,   /**< Push inline property */
    N00B_RICH_PROP_OFF,  /**< Pop inline property */
    N00B_RICH_RESET,     /**< Clear style stack */
    N00B_RICH_SUBST,     /**< Format substitution */
    N00B_RICH_ROLE_ON,   /**< Push text role */
    N00B_RICH_ROLE_OFF,  /**< Pop text role */
} n00b_rich_seg_kind_t;

// ===================================================================
// Parsed segment
// ===================================================================

/** @brief One segment of a parsed rich markup descriptor. */
typedef struct {
    n00b_rich_seg_kind_t kind;

    /** For TEXT: start offset in descriptor.  For SUBST: arg index. */
    int32_t offset;

    /** For TEXT: byte length.  For SUBST: -1 = auto-index. */
    int32_t length;

    /** Tag name (for STYLE_ON/OFF, PROP_ON/OFF, ROLE_ON/OFF).
     *  Format spec string (for SUBST with spec).  nullptr otherwise. */
    const char *tag;

    /** True if SUBST has the `!` strip-styling flag. */
    bool strip_style;
} n00b_rich_segment_t;

// ===================================================================
// Parsed descriptor
// ===================================================================

/** @brief A pre-parsed rich markup descriptor (cacheable). */
typedef struct {
    int32_t              num_segments;
    n00b_rich_segment_t  segments[];
} n00b_rich_desc_t;

// ===================================================================
// Public API
// ===================================================================

/** @brief Parse a rich markup descriptor string.
 *
 *  Results are cached by XXH3 hash of the input bytes.  Subsequent
 *  calls with the same descriptor return the cached parse.
 *
 *  @param desc      UTF-8 descriptor bytes (not necessarily NUL-terminated).
 *  @param desc_len  Length in bytes.
 *  @return Parsed descriptor (owned by cache; do not free).
 */
n00b_rich_desc_t *n00b_rich_desc_parse(const char *desc, int32_t desc_len);

/** @brief Initialize the rich descriptor cache.
 *
 *  Called by `n00b_init()` (via `n00b_str_registry_init()` or
 *  separately).
 */
void n00b_rich_desc_cache_init(void);
