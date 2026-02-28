// Simulates a second header that uses n00b_result_t(int).
// This would fail with n00b_result_decl() if both headers are included,
// but _generic_struct deduplicates automatically.

#pragma once

#include "test_gs_result.h"

// This expansion produces _generic_struct <tag> { ... } a second time.
// _generic_struct deduplicates it — only the first definition survives.
static inline n00b_result_t(int)
double_result(n00b_result_t(int) r)
{
    if (n00b_result_is_ok(r)) {
        return n00b_result_ok(int, n00b_result_get(r) * 2);
    }
    return r;
}
