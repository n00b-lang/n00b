/**
 * @file test/unit/test_regex_neon_simd.h
 * @brief Companion header for `test_regex_neon_simd.c` (Phase 11 port of
 *        `resharp-c/tests/neon_simd_test.{c,h}`).
 *
 * The two helpers `regex_find_all_alloc` and `regex_is_match_b` were
 * declared in the source-of-truth `neon_simd_test.h` but **never called
 * from `neon_simd_test.c`** — a verification step confirmed via grep
 * across all of `~/resharp-c/`.  They were forward-compat shims for a
 * shared-test layer that never materialised.  The port does not need
 * them: every match site in `test_regex_neon_simd.c` uses the engine
 * surface (`regex_find_all` filling an `n00b_list_t(Match)`, plus
 * `regex_is_match` with a bool out-param) directly.
 *
 * The header is kept (rather than dropped) per the 1:1 file-mapping
 * audit rule in Phase 11.  It carries no declarations of its own.
 */
#pragma once

#include "n00b.h"

#include "internal/regex/regex.h"
#include "internal/regex/accel.h"
