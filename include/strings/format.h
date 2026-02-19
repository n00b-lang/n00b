#pragma once
/** @file format.h
 *  @brief `n00b_format()` — rich markup formatting with substitutions.
 *
 *  The capstone API for the rich string system.  Takes a markup
 *  descriptor and variadic arguments, returns a styled `n00b_string_t`
 *  with abstract styling metadata (no ANSI codes, no color resolution).
 *
 *  ### Usage
 *
 *  ```c
 *  n00b_string_t s = n00b_format(STR("[|b|]Hello[|/b|] [|#|]!"), &name);
 *  ```
 *
 *  ### Related modules
 *
 *  - `strings/rich_desc.h` -- descriptor parsing / caching
 *  - `strings/format_spec.h` -- spec parsing / formatting
 *  - `strings/style_registry.h` -- named style / role lookup
 *  - `strings/string_style.h` -- style attachment
 */

#include "strings/string_style.h"
#include "strings/rich_desc.h"
#include "strings/format_spec.h"
#include "strings/style_registry.h"
#include "core/vargs.h"

// ===================================================================
// Public API
// ===================================================================

/** @brief Format a rich markup descriptor with variadic arguments.
 *
 *  Parses the descriptor (cached), walks segments, resolves styles
 *  and roles from the registry, formats substitutions, and returns
 *  a styled `n00b_string_t`.
 *
 *  Arguments are consumed positionally (auto-indexed `[|#|]`) or by
 *  explicit index (`[|#N|]`).  Integer args are passed as `int64_t`
 *  via cast to `void *`.  String args are passed as `n00b_string_t *`.
 *  Double args are passed as `double *`.
 *
 *  @param desc  The markup descriptor string.
 *  @param +     Variadic substitution arguments.
 *  @return A styled `n00b_string_t` (by value).
 */
n00b_string_t n00b_format(n00b_string_t desc, +);

/** @brief Convenience: format from a C string descriptor.
 *
 *  Wraps the C string in an `n00b_string_t` and calls `n00b_format()`.
 *
 *  @param desc  NUL-terminated C string descriptor.
 *  @param +     Variadic substitution arguments.
 *  @return A styled `n00b_string_t` (by value).
 */
n00b_string_t n00b_cformat(const char *desc, +);
