#pragma once
/** @file properties.h
 *  @brief Per-codepoint Unicode property lookups and string-level display width.
 *
 *  Provides access to General_Category, Canonical_Combining_Class, Script,
 *  Block, Bidi_Class, East_Asian_Width, Joining_Type, binary properties,
 *  numeric type/value, and string display width measurement.
 */

#include "text/unicode/types_ext.h"

// ===========================================================================
// Per-codepoint property lookups
// ===========================================================================

/** @brief Return the General_Category of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The General_Category value.
 */
n00b_unicode_gc_t n00b_unicode_general_category(n00b_codepoint_t cp);

/** @brief Return the Canonical_Combining_Class of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The combining class (0..254).
 */
uint8_t n00b_unicode_combining_class(n00b_codepoint_t cp);

/** @brief Return the Script property of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return A script index (use n00b_unicode_script_name() for the name).
 */
n00b_unicode_script_t n00b_unicode_script(n00b_codepoint_t cp);

/** @brief Return the human-readable name of a script value.
 *  @param s  A script index returned by n00b_unicode_script().
 *  @return A static string such as "Latin", "Han", etc.
 */
const char *n00b_unicode_script_name(n00b_unicode_script_t s);

/** @brief Return the Block property of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return A block index (use n00b_unicode_block_name() for the name).
 */
n00b_unicode_block_t n00b_unicode_block(n00b_codepoint_t cp);

/** @brief Return the human-readable name of a block value.
 *  @param b  A block index returned by n00b_unicode_block().
 *  @return A static string such as "Basic Latin", etc.
 */
const char *n00b_unicode_block_name(n00b_unicode_block_t b);

/** @brief Return the Bidi_Class of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The bidirectional class value.
 */
n00b_unicode_bidi_class_t n00b_unicode_bidi_class(n00b_codepoint_t cp);

/** @brief Return the East_Asian_Width property of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The East Asian width category.
 */
n00b_unicode_eaw_t n00b_unicode_east_asian_width(n00b_codepoint_t cp);

/** @brief Return the display column width of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return 0, 1, or 2 columns.
 */
int n00b_unicode_char_width(n00b_codepoint_t cp);

/** @brief Return the Joining_Type of a codepoint (for Arabic shaping).
 *  @param cp  The codepoint to query.
 *  @return The joining type value.
 */
n00b_unicode_jt_t n00b_unicode_joining_type(n00b_codepoint_t cp);

/** @brief Test whether a codepoint has a given binary property.
 *  @param cp    The codepoint to query.
 *  @param prop  The binary property to test.
 *  @return true if the codepoint has the property.
 */
bool n00b_unicode_has_property(n00b_codepoint_t cp,
                               n00b_unicode_property_t prop);

/** @brief Retrieve the Script_Extensions for a codepoint (UTS #24).
 *  @param cp           The codepoint to query.
 *  @param scripts      Output array for script values.
 *  @param max_scripts  Maximum number of scripts to write.
 *  @return Number of scripts written, or total count if > max_scripts.
 */
int n00b_unicode_script_extensions(n00b_codepoint_t cp,
                                   n00b_unicode_script_t *scripts,
                                   int max_scripts);

// ===========================================================================
// Numeric properties
// ===========================================================================

/** @brief Return the Numeric_Type of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The numeric type (NONE, DECIMAL, DIGIT, or NUMERIC).
 */
n00b_unicode_numeric_type_t n00b_unicode_numeric_type(n00b_codepoint_t cp);

/** @brief Return the full Numeric_Value of a codepoint as a rational.
 *  @param cp  The codepoint to query.
 *  @return A numeric value struct with type, numerator, and denominator.
 */
n00b_unicode_numeric_value_t n00b_unicode_numeric_value(n00b_codepoint_t cp);

/** @brief Return the decimal digit value (0..9) of a codepoint, if any.
 *  @param cp  The codepoint to query.
 *  @return An option containing the digit value, or none.
 */
n00b_option_t(int32_t) n00b_unicode_digit_value(n00b_codepoint_t cp);

// ===========================================================================
// String-level display width
// ===========================================================================

/** @brief Compute the total display width of a UTF-8 string in columns.
 *  @param s  The string to measure.
 *  @return The display width (sum of character widths).
 */
int32_t n00b_unicode_display_width(n00b_string_t *s);

// ===========================================================================
// Age
// ===========================================================================
//
// The Age property gives the Unicode version in which a codepoint was first
// assigned (UAX #44 DerivedAge.txt).  Index 0 is the sentinel "Unassigned"
// bucket; non-zero indices map into the age-name table in version order.
// ===========================================================================

/** @brief Unicode Age value (index into the age-name table; 0 = Unassigned). */
typedef uint8_t n00b_unicode_age_t;

/** @brief Return the Age (Unicode version index) of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return Index into the age-name table (0 = "Unassigned").
 */
n00b_unicode_age_t n00b_unicode_age(n00b_codepoint_t cp);

/** @brief Return the human-readable name of an age value.
 *  @param age  An age index returned by n00b_unicode_age().
 *  @return A static string such as "12.0", or "Unassigned" for index 0 or
 *          out-of-range inputs.
 */
const char *n00b_unicode_age_name(n00b_unicode_age_t age);

/** @brief Total number of Age entries (length of the age-name table,
 *         including the sentinel "Unassigned" bucket at index 0).
 *
 *  This is a generated constant emitted by tools/gen_tables.py alongside
 *  the age tables, exposed here for symmetry with n00b_unicode_script_count
 *  / n00b_unicode_block_count.
 */
extern const uint32_t n00b_unicode_age_count;

// ===========================================================================
// Range enumeration
// ===========================================================================
//
// Turn property predicates into sorted, merged [lo, hi] codepoint range
// arrays.  All `*_ranges` accessors return pointers into static, lazy-
// initialised buffers; the data is valid for the program lifetime and must
// not be freed.  On first call for a given property class the buffers are
// populated by a single sweep over 0..0x10FFFF (one sweep per class — gc /
// script / scx / block / property / bidi / age), so steady-state cost is
// O(1) memory + O(#ranges).
//
// The returned ranges are:
//   - sorted ascending by `lo`,
//   - non-overlapping,
//   - non-contiguous (adjacent codepoints are merged), with the standard
//     surrogate hole [D800..DFFF] excluded entirely.
// ===========================================================================

// `n00b_codepoint_pair_t` is defined in `text/unicode/types.h` (transitively
// included via `types_ext.h` above).

/** @brief Get the codepoint ranges for a single General_Category value.
 *  @param gc          GC enum value (N00B_UNICODE_GC_LU..N00B_UNICODE_GC_CN).
 *  @param out_ranges  Out-pointer to a static, sorted, merged range array.
 *  @param out_len     Out-pointer to the array length.
 */
void n00b_unicode_general_category_ranges(n00b_unicode_gc_t gc,
                                          const n00b_codepoint_pair_t **out_ranges,
                                          size_t *out_len);

/** @brief Get the codepoint ranges for a single Script value.
 *  @param sc          Script enum value.
 *  @param out_ranges  Out-pointer to a static, sorted, merged range array.
 *  @param out_len     Out-pointer to the array length.
 */
void n00b_unicode_script_ranges(n00b_unicode_script_t sc,
                                const n00b_codepoint_pair_t **out_ranges,
                                size_t *out_len);

/** @brief Get the codepoint ranges of all codepoints whose Script_Extensions
 *         set contains @p sc (UTS #24).
 *  @param sc          Script enum value.
 *  @param out_ranges  Out-pointer to a static, sorted, merged range array.
 *  @param out_len     Out-pointer to the array length.
 */
void n00b_unicode_script_extensions_ranges(n00b_unicode_script_t sc,
                                           const n00b_codepoint_pair_t **out_ranges,
                                           size_t *out_len);

/** @brief Get the codepoint ranges for a single Block value (UAX #44 Blocks).
 *  @param bl          Block id (0 = No_Block).
 *  @param out_ranges  Out-pointer to a static, sorted, merged range array.
 *  @param out_len     Out-pointer to the array length.
 */
void n00b_unicode_block_ranges_for(n00b_unicode_block_t bl,
                                   const n00b_codepoint_pair_t **out_ranges,
                                   size_t *out_len);

/** @brief Get the codepoint ranges for a binary property.
 *  @param prop        The binary property to query.
 *  @param out_ranges  Out-pointer to a static, sorted, merged range array.
 *  @param out_len     Out-pointer to the array length.
 */
void n00b_unicode_property_ranges(n00b_unicode_property_t prop,
                                  const n00b_codepoint_pair_t **out_ranges,
                                  size_t *out_len);

/** @brief Get the codepoint ranges for a single Bidi_Class value.
 *  @param bc          Bidi_Class enum value.
 *  @param out_ranges  Out-pointer to a static, sorted, merged range array.
 *  @param out_len     Out-pointer to the array length.
 */
void n00b_unicode_bidi_class_ranges(n00b_unicode_bidi_class_t bc,
                                    const n00b_codepoint_pair_t **out_ranges,
                                    size_t *out_len);

/** @brief Get the codepoint ranges for a derived (composite) GC name.
 *
 *  Derived GCs are unions of base GCs:
 *    Letter (L)             = Lu | Ll | Lt | Lm | Lo
 *    Cased_Letter (LC)      = Lu | Ll | Lt
 *    Mark (M)               = Mn | Mc | Me
 *    Number (N)             = Nd | Nl | No
 *    Punctuation (P)        = Pc | Pd | Ps | Pe | Pi | Pf | Po
 *    Symbol (S)             = Sm | Sc | Sk | So
 *    Separator (Z)          = Zs | Zl | Zp
 *    Other (C)              = Cc | Cf | Cs | Co | Cn
 *
 *  Matching is loose (UAX #44 LM3): "Letter", "letter", "L", "Cased_Letter",
 *  "casedletter", and "LC" all map to the same set.
 *
 *  The base GCs (e.g. "Lu", "Uppercase_Letter") are NOT handled here — call
 *  n00b_unicode_general_category_ranges() with the resolved enum instead.
 *
 *  @param name        Derived-GC name (loose match).
 *  @param out_ranges  Out-pointer to a static, sorted, merged range array.
 *  @param out_len     Out-pointer to the array length.
 *  @return true iff @p name resolved to a known derived GC.
 */
bool n00b_unicode_gc_derived_ranges(const char *name,
                                    const n00b_codepoint_pair_t **out_ranges,
                                    size_t *out_len);

/** @brief Get the codepoint ranges for the cumulative Age <= named version.
 *
 *  Per UAX #44 / regex-syntax, an Age query of "1.1" returns *all codepoints
 *  assigned in Unicode 1.1 or earlier*.  Accepts either the canonical
 *  underscore form ("V12_0") or the dotted form ("12.0").
 *
 *  @param name        Age name (loose match; accepts "V12_0" or "12.0").
 *  @param out_ranges  Out-pointer to a static, sorted, merged range array.
 *  @param out_len     Out-pointer to the array length.
 *  @return true iff @p name resolved.
 */
bool n00b_unicode_age_ranges(const char *name,
                             const n00b_codepoint_pair_t **out_ranges,
                             size_t *out_len);
