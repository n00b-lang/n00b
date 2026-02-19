#pragma once
/** @file fptostr.h
 *  @brief Grisu2-based double-to-string conversion.
 *
 *  Converts IEEE 754 doubles to their shortest decimal string
 *  representation using the Grisu2 algorithm.  Handles special values
 *  (NaN, ±Inf, ±0) and writes directly to a caller-provided buffer.
 *
 *  Based on the fpconv implementation by Andreas Samoljuk (MIT license),
 *  which is itself an implementation of Florian Loitsch's Grisu2 algorithm.
 *
 *  ### Related modules
 *
 *  - `strings/fmt_numbers.h` -- higher-level float formatting that uses this
 *
 *  @see https://github.com/night-shift/fpconv
 *  @see http://florian.loitsch.com/publications/dtoa-pldi2010.pdf
 */

#include <stdint.h>

/**
 * @brief Convert a double to its shortest decimal string representation.
 *
 * Uses the Grisu2 algorithm.  Handles ±0, NaN, ±Inf.
 *
 * @param d     The value to convert.
 * @param dest  Output buffer; must hold at least 24 bytes.
 * @return      Number of characters written (not NUL-terminated).
 *
 * @pre  `dest` points to a buffer of at least 24 bytes.
 * @post `dest[0..return)` contains the decimal representation.
 */
int n00b_fptostr(double d, char dest[24]);
