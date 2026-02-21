#include "logic/clpfd_domain.h"
#include "n00b.h"
#include "core/alloc.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int
cmp_i64(const void *a, const void *b)
{
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;

    return (va > vb) - (va < vb);
}

static int
popcount64(uint64_t x)
{
    return __builtin_popcountll(x);
}

static int
ctz64(uint64_t x)
{
    return __builtin_ctzll(x);
}

static int
clz64(uint64_t x)
{
    return __builtin_clzll(x);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

n00b_csp_domain_t
n00b_csp_dom_range(int64_t lo, int64_t hi)
{
    n00b_csp_domain_t d;

    if (lo > hi) {
        d.kind = N00B_CSP_DOM_EMPTY;
        return d;
    }

    d.kind        = N00B_CSP_DOM_INTERVAL;
    d.interval.lo = lo;
    d.interval.hi = hi;

    return d;
}

n00b_csp_domain_t
n00b_csp_dom_singleton(int64_t val)
{
    return n00b_csp_dom_range(val, val);
}

n00b_csp_domain_t
n00b_csp_dom_from_values(const int64_t *vals, int32_t count)
{
    n00b_csp_domain_t d;

    if (count <= 0) {
        d.kind = N00B_CSP_DOM_EMPTY;
        return d;
    }

    // Copy and sort.
    int64_t *sorted = n00b_alloc_array(int64_t, count);
    memcpy(sorted, vals, count * sizeof(int64_t));
    qsort(sorted, count, sizeof(int64_t), cmp_i64);

    // Deduplicate.
    int32_t unique = 1;

    for (int32_t i = 1; i < count; i++) {
        if (sorted[i] != sorted[unique - 1]) {
            sorted[unique++] = sorted[i];
        }
    }

    // Check if it's a contiguous interval.
    if (sorted[unique - 1] - sorted[0] == unique - 1) {
        int64_t lo = sorted[0];
        int64_t hi = sorted[unique - 1];
        n00b_free(sorted);
        return n00b_csp_dom_range(lo, hi);
    }

    // Check if bitset representation works (range <= 64).
    int64_t range = sorted[unique - 1] - sorted[0] + 1;

    if (range <= 64) {
        d.kind        = N00B_CSP_DOM_BITSET;
        d.bitset.base = sorted[0];
        d.bitset.bits = 0;

        for (int32_t i = 0; i < unique; i++) {
            d.bitset.bits |= (uint64_t)1 << (sorted[i] - sorted[0]);
        }

        n00b_free(sorted);
        return d;
    }

    // Sparse.
    d.kind          = N00B_CSP_DOM_SPARSE;
    d.sparse.values = sorted;
    d.sparse.count  = unique;
    d.sparse.cap    = count;

    return d;
}

n00b_csp_domain_t
n00b_csp_dom_empty(void)
{
    n00b_csp_domain_t d;
    d.kind = N00B_CSP_DOM_EMPTY;

    return d;
}

n00b_csp_domain_t
n00b_csp_dom_clone(const n00b_csp_domain_t *d)
{
    n00b_csp_domain_t c = *d;

    if (d->kind == N00B_CSP_DOM_SPARSE && d->sparse.values) {
        c.sparse.values = n00b_alloc_array(int64_t, d->sparse.count);
        memcpy(c.sparse.values, d->sparse.values,
               d->sparse.count * sizeof(int64_t));
        c.sparse.cap = d->sparse.count;
    }

    return c;
}

void
n00b_csp_dom_free(n00b_csp_domain_t *d)
{
    if (d->kind == N00B_CSP_DOM_SPARSE) {
        n00b_free(d->sparse.values);
        d->sparse.values = nullptr;
        d->sparse.count  = 0;
        d->sparse.cap    = 0;
    }

    d->kind = N00B_CSP_DOM_EMPTY;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

int64_t
n00b_csp_dom_min(const n00b_csp_domain_t *d)
{
    switch (d->kind) {
    case N00B_CSP_DOM_INTERVAL:
        return d->interval.lo;
    case N00B_CSP_DOM_BITSET:
        return d->bitset.base + ctz64(d->bitset.bits);
    case N00B_CSP_DOM_SPARSE:
        return d->sparse.count > 0 ? d->sparse.values[0] : 0;
    case N00B_CSP_DOM_EMPTY:
        return 0;
    }

    return 0;
}

int64_t
n00b_csp_dom_max(const n00b_csp_domain_t *d)
{
    switch (d->kind) {
    case N00B_CSP_DOM_INTERVAL:
        return d->interval.hi;
    case N00B_CSP_DOM_BITSET:
        return d->bitset.base + (63 - clz64(d->bitset.bits));
    case N00B_CSP_DOM_SPARSE:
        return d->sparse.count > 0
                   ? d->sparse.values[d->sparse.count - 1]
                   : 0;
    case N00B_CSP_DOM_EMPTY:
        return 0;
    }

    return 0;
}

int64_t
n00b_csp_dom_size(const n00b_csp_domain_t *d)
{
    switch (d->kind) {
    case N00B_CSP_DOM_INTERVAL:
        return d->interval.hi - d->interval.lo + 1;
    case N00B_CSP_DOM_BITSET:
        return popcount64(d->bitset.bits);
    case N00B_CSP_DOM_SPARSE:
        return d->sparse.count;
    case N00B_CSP_DOM_EMPTY:
        return 0;
    }

    return 0;
}

bool
n00b_csp_dom_is_singleton(const n00b_csp_domain_t *d)
{
    return n00b_csp_dom_size(d) == 1;
}

bool
n00b_csp_dom_is_empty(const n00b_csp_domain_t *d)
{
    return d->kind == N00B_CSP_DOM_EMPTY
           || (d->kind == N00B_CSP_DOM_BITSET && d->bitset.bits == 0)
           || (d->kind == N00B_CSP_DOM_SPARSE && d->sparse.count == 0);
}

bool
n00b_csp_dom_contains(const n00b_csp_domain_t *d, int64_t val)
{
    switch (d->kind) {
    case N00B_CSP_DOM_INTERVAL:
        return val >= d->interval.lo && val <= d->interval.hi;
    case N00B_CSP_DOM_BITSET: {
        int64_t offset = val - d->bitset.base;

        if (offset < 0 || offset > 63) {
            return false;
        }

        return (d->bitset.bits >> offset) & 1;
    }
    case N00B_CSP_DOM_SPARSE: {
        int32_t lo = 0, hi = d->sparse.count - 1;

        while (lo <= hi) {
            int32_t mid = lo + (hi - lo) / 2;

            if (d->sparse.values[mid] == val) {
                return true;
            }

            if (d->sparse.values[mid] < val) {
                lo = mid + 1;
            }
            else {
                hi = mid - 1;
            }
        }

        return false;
    }
    case N00B_CSP_DOM_EMPTY:
        return false;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Interval -> bitset/sparse promotion
// ---------------------------------------------------------------------------

static n00b_csp_domain_t
interval_to_bitset(int64_t lo, int64_t hi)
{
    n00b_csp_domain_t d;
    d.kind        = N00B_CSP_DOM_BITSET;
    d.bitset.base = lo;
    d.bitset.bits = 0;

    for (int64_t v = lo; v <= hi; v++) {
        d.bitset.bits |= (uint64_t)1 << (v - lo);
    }

    return d;
}

static n00b_csp_domain_t
interval_to_sparse(int64_t lo, int64_t hi)
{
    int32_t           count = (int32_t)(hi - lo + 1);
    n00b_csp_domain_t d;

    d.kind          = N00B_CSP_DOM_SPARSE;
    d.sparse.values = n00b_alloc_array(int64_t, count);
    d.sparse.count  = count;
    d.sparse.cap    = count;

    for (int32_t i = 0; i < count; i++) {
        d.sparse.values[i] = lo + i;
    }

    return d;
}

// ---------------------------------------------------------------------------
// Narrowing
// ---------------------------------------------------------------------------

bool
n00b_csp_dom_intersect(n00b_csp_domain_t *d, const n00b_csp_domain_t *other)
{
    if (d->kind == N00B_CSP_DOM_EMPTY) {
        return false;
    }

    if (other->kind == N00B_CSP_DOM_EMPTY) {
        n00b_csp_dom_free(d);
        d->kind = N00B_CSP_DOM_EMPTY;
        return true;
    }

    // Interval ∩ Interval
    if (d->kind == N00B_CSP_DOM_INTERVAL && other->kind == N00B_CSP_DOM_INTERVAL) {
        int64_t new_lo = d->interval.lo > other->interval.lo
                             ? d->interval.lo
                             : other->interval.lo;
        int64_t new_hi = d->interval.hi < other->interval.hi
                             ? d->interval.hi
                             : other->interval.hi;

        if (new_lo > new_hi) {
            d->kind = N00B_CSP_DOM_EMPTY;
            return true;
        }

        bool changed = (new_lo != d->interval.lo || new_hi != d->interval.hi);
        d->interval.lo = new_lo;
        d->interval.hi = new_hi;

        return changed;
    }

    // Bitset ∩ Bitset (possibly different bases)
    if (d->kind == N00B_CSP_DOM_BITSET && other->kind == N00B_CSP_DOM_BITSET) {
        int64_t d_max      = d->bitset.base + 63;
        int64_t o_max      = other->bitset.base + 63;
        int64_t overlap_lo = d->bitset.base > other->bitset.base
                                 ? d->bitset.base
                                 : other->bitset.base;
        int64_t overlap_hi = d_max < o_max ? d_max : o_max;

        if (overlap_lo > overlap_hi) {
            uint64_t old   = d->bitset.bits;
            d->bitset.bits = 0;
            d->kind        = N00B_CSP_DOM_EMPTY;
            return old != 0;
        }

        uint64_t d_shifted = d->bitset.bits >> (overlap_lo - d->bitset.base);
        uint64_t o_shifted =
            other->bitset.bits >> (overlap_lo - other->bitset.base);

        int64_t  range = overlap_hi - overlap_lo + 1;
        uint64_t mask =
            range >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << range) - 1;

        uint64_t new_bits = d_shifted & o_shifted & mask;
        bool changed =
            (new_bits != d->bitset.bits || overlap_lo != d->bitset.base);

        d->bitset.base = overlap_lo;
        d->bitset.bits = new_bits;

        if (new_bits == 0) {
            d->kind = N00B_CSP_DOM_EMPTY;
        }

        return changed;
    }

    // General case: build result via membership test.
    // Collect values from d that are also in other using a dynamic buffer.
    int64_t *tmp       = nullptr;
    int32_t  tmp_count = 0;
    int32_t  tmp_cap   = 0;

    switch (d->kind) {
    case N00B_CSP_DOM_INTERVAL:
        for (int64_t v = d->interval.lo; v <= d->interval.hi; v++) {
            if (n00b_csp_dom_contains(other, v)) {
                if (tmp_count >= tmp_cap) {
                    int32_t  new_cap = tmp_cap ? tmp_cap * 2 : 16;
                    int64_t *new_tmp = n00b_alloc_array(int64_t, new_cap);
                    if (tmp_count > 0) {
                        memcpy(new_tmp, tmp, tmp_count * sizeof(int64_t));
                    }
                    n00b_free(tmp);
                    tmp     = new_tmp;
                    tmp_cap = new_cap;
                }
                tmp[tmp_count++] = v;
            }
        }
        break;
    case N00B_CSP_DOM_BITSET:
        for (uint64_t bits = d->bitset.bits; bits;) {
            int     bit = ctz64(bits);
            int64_t v   = d->bitset.base + bit;

            if (n00b_csp_dom_contains(other, v)) {
                if (tmp_count >= tmp_cap) {
                    int32_t  new_cap = tmp_cap ? tmp_cap * 2 : 16;
                    int64_t *new_tmp = n00b_alloc_array(int64_t, new_cap);
                    if (tmp_count > 0) {
                        memcpy(new_tmp, tmp, tmp_count * sizeof(int64_t));
                    }
                    n00b_free(tmp);
                    tmp     = new_tmp;
                    tmp_cap = new_cap;
                }
                tmp[tmp_count++] = v;
            }

            bits &= bits - 1;
        }
        break;
    case N00B_CSP_DOM_SPARSE:
        for (int32_t i = 0; i < d->sparse.count; i++) {
            if (n00b_csp_dom_contains(other, d->sparse.values[i])) {
                if (tmp_count >= tmp_cap) {
                    int32_t  new_cap = tmp_cap ? tmp_cap * 2 : 16;
                    int64_t *new_tmp = n00b_alloc_array(int64_t, new_cap);
                    if (tmp_count > 0) {
                        memcpy(new_tmp, tmp, tmp_count * sizeof(int64_t));
                    }
                    n00b_free(tmp);
                    tmp     = new_tmp;
                    tmp_cap = new_cap;
                }
                tmp[tmp_count++] = d->sparse.values[i];
            }
        }
        break;
    default:
        break;
    }

    int64_t old_size = n00b_csp_dom_size(d);
    n00b_csp_dom_free(d);

    if (tmp_count == 0) {
        n00b_free(tmp);
        d->kind = N00B_CSP_DOM_EMPTY;
        return old_size > 0;
    }

    *d = n00b_csp_dom_from_values(tmp, tmp_count);
    n00b_free(tmp);

    return tmp_count != old_size;
}

bool
n00b_csp_dom_remove_value(n00b_csp_domain_t *d, int64_t val)
{
    if (!n00b_csp_dom_contains(d, val)) {
        return false;
    }

    switch (d->kind) {
    case N00B_CSP_DOM_INTERVAL:
        if (val == d->interval.lo && val == d->interval.hi) {
            d->kind = N00B_CSP_DOM_EMPTY;
            return true;
        }

        if (val == d->interval.lo) {
            d->interval.lo++;
            return true;
        }

        if (val == d->interval.hi) {
            d->interval.hi--;
            return true;
        }

        // Hole in the middle — promote to bitset or sparse.
        {
            int64_t range = d->interval.hi - d->interval.lo + 1;

            if (range <= 64) {
                *d = interval_to_bitset(d->interval.lo, d->interval.hi);
                d->bitset.bits &= ~((uint64_t)1 << (val - d->bitset.base));
                return true;
            }

            *d = interval_to_sparse(d->interval.lo, d->interval.hi);
            // Fall through to sparse removal.
        }
        [[fallthrough]];
    case N00B_CSP_DOM_SPARSE: {
        int32_t lo_idx = 0, hi_idx = d->sparse.count - 1;

        while (lo_idx <= hi_idx) {
            int32_t mid = lo_idx + (hi_idx - lo_idx) / 2;

            if (d->sparse.values[mid] == val) {
                memmove(&d->sparse.values[mid],
                        &d->sparse.values[mid + 1],
                        (d->sparse.count - mid - 1) * sizeof(int64_t));
                d->sparse.count--;

                if (d->sparse.count == 0) {
                    d->kind = N00B_CSP_DOM_EMPTY;
                }

                return true;
            }

            if (d->sparse.values[mid] < val) {
                lo_idx = mid + 1;
            }
            else {
                hi_idx = mid - 1;
            }
        }

        return false;
    }
    case N00B_CSP_DOM_BITSET: {
        int64_t offset = val - d->bitset.base;
        d->bitset.bits &= ~((uint64_t)1 << offset);

        if (d->bitset.bits == 0) {
            d->kind = N00B_CSP_DOM_EMPTY;
        }

        return true;
    }
    case N00B_CSP_DOM_EMPTY:
        return false;
    }

    return false;
}

bool
n00b_csp_dom_restrict_min(n00b_csp_domain_t *d, int64_t new_min)
{
    if (n00b_csp_dom_is_empty(d)) {
        return false;
    }

    int64_t cur_min = n00b_csp_dom_min(d);

    if (new_min <= cur_min) {
        return false;
    }

    switch (d->kind) {
    case N00B_CSP_DOM_INTERVAL:
        if (new_min > d->interval.hi) {
            d->kind = N00B_CSP_DOM_EMPTY;
            return true;
        }

        d->interval.lo = new_min;
        return true;

    case N00B_CSP_DOM_BITSET: {
        int64_t offset = new_min - d->bitset.base;

        if (offset > 63) {
            d->bitset.bits = 0;
            d->kind        = N00B_CSP_DOM_EMPTY;
            return true;
        }

        if (offset > 0) {
            uint64_t mask = ~(((uint64_t)1 << offset) - 1);
            uint64_t old  = d->bitset.bits;
            d->bitset.bits &= mask;

            if (d->bitset.bits == 0) {
                d->kind = N00B_CSP_DOM_EMPTY;
            }

            return d->bitset.bits != old;
        }

        return false;
    }
    case N00B_CSP_DOM_SPARSE: {
        int32_t i = 0;

        while (i < d->sparse.count && d->sparse.values[i] < new_min) {
            i++;
        }

        if (i == d->sparse.count) {
            d->sparse.count = 0;
            d->kind         = N00B_CSP_DOM_EMPTY;
            return true;
        }

        if (i > 0) {
            memmove(d->sparse.values,
                    &d->sparse.values[i],
                    (d->sparse.count - i) * sizeof(int64_t));
            d->sparse.count -= i;
            return true;
        }

        return false;
    }
    default:
        return false;
    }
}

bool
n00b_csp_dom_restrict_max(n00b_csp_domain_t *d, int64_t new_max)
{
    if (n00b_csp_dom_is_empty(d)) {
        return false;
    }

    int64_t cur_max = n00b_csp_dom_max(d);

    if (new_max >= cur_max) {
        return false;
    }

    switch (d->kind) {
    case N00B_CSP_DOM_INTERVAL:
        if (new_max < d->interval.lo) {
            d->kind = N00B_CSP_DOM_EMPTY;
            return true;
        }

        d->interval.hi = new_max;
        return true;

    case N00B_CSP_DOM_BITSET: {
        int64_t offset = new_max - d->bitset.base;

        if (offset < 0) {
            d->bitset.bits = 0;
            d->kind        = N00B_CSP_DOM_EMPTY;
            return true;
        }

        uint64_t mask = ((uint64_t)1 << (offset + 1)) - 1;
        uint64_t old  = d->bitset.bits;
        d->bitset.bits &= mask;

        if (d->bitset.bits == 0) {
            d->kind = N00B_CSP_DOM_EMPTY;
        }

        return d->bitset.bits != old;
    }
    case N00B_CSP_DOM_SPARSE: {
        int32_t i = d->sparse.count - 1;

        while (i >= 0 && d->sparse.values[i] > new_max) {
            i--;
        }

        int32_t new_count = i + 1;

        if (new_count == 0) {
            d->sparse.count = 0;
            d->kind         = N00B_CSP_DOM_EMPTY;
            return true;
        }

        if (new_count < d->sparse.count) {
            d->sparse.count = new_count;
            return true;
        }

        return false;
    }
    default:
        return false;
    }
}
