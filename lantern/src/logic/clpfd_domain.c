#include "logic/clpfd_domain.h"
#include "lib/alloc.h"

#include <string.h>

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

core_csp_domain_t
core_csp_dom_range(int64_t lo, int64_t hi)
{
    if (lo > hi) {
        return core_variant_empty(core_csp_domain_t);
    }

    return core_csp_domain_set(core_csp_domain_t,
                            core_csp_dom_interval_t,
                            ((core_csp_dom_interval_t){
                                .lo = lo,
                                .hi = hi,
                            }));
}

core_csp_domain_t
core_csp_dom_singleton(int64_t val)
{
    return core_csp_dom_range(val, val);
}

core_csp_domain_t
core_csp_dom_from_values(const int64_t *vals, int32_t count)
{
    if (count <= 0) {
        return core_variant_empty(core_csp_domain_t);
    }

    int64_t *sorted = core_alloc_array(int64_t, count);
    memcpy(sorted, vals, count * sizeof(int64_t));
    qsort(sorted, count, sizeof(int64_t), cmp_i64);

    int32_t unique = 1;

    for (int32_t i = 1; i < count; i++) {
        if (sorted[i] != sorted[unique - 1]) {
            sorted[unique++] = sorted[i];
        }
    }

    uint64_t span = (uint64_t)sorted[unique - 1] - (uint64_t)sorted[0];

    if (span == (uint64_t)(unique - 1)) {
        int64_t lo = sorted[0];
        int64_t hi = sorted[unique - 1];
        core_free(sorted);
        return core_csp_dom_range(lo, hi);
    }

    if (span < 64) {
        core_csp_dom_bitset_t bs = {
            .base = sorted[0],
            .bits = 0,
        };

        for (int32_t i = 0; i < unique; i++) {
            bs.bits |= (uint64_t)1 <<
                       ((uint64_t)sorted[i] - (uint64_t)sorted[0]);
        }

        core_free(sorted);
        return core_csp_domain_set(core_csp_domain_t, core_csp_dom_bitset_t, bs);
    }

    return core_csp_domain_set(core_csp_domain_t,
                            core_csp_dom_sparse_t,
                            ((core_csp_dom_sparse_t){
                                .values = sorted,
                                .count  = unique,
                                .cap    = count,
                            }));
}

core_csp_domain_t
core_csp_dom_empty(void)
{
    return core_variant_empty(core_csp_domain_t);
}

core_csp_domain_t
core_csp_dom_clone(const core_csp_domain_t *d)
{
    core_csp_domain_t c = *d;

    if (core_csp_domain_is_type(*d, core_csp_dom_sparse_t)) {
        core_csp_dom_sparse_t sp = core_csp_domain_get(*d, core_csp_dom_sparse_t);

        if (sp.values) {
            int64_t *copy = core_alloc_array(int64_t, sp.count);
            memcpy(copy, sp.values, sp.count * sizeof(int64_t));
            c = core_csp_domain_set(core_csp_domain_t,
                                 core_csp_dom_sparse_t,
                                 ((core_csp_dom_sparse_t){
                                     .values = copy,
                                     .count  = sp.count,
                                     .cap    = sp.count,
                                 }));
        }
    }

    return c;
}

void
core_csp_dom_free(core_csp_domain_t *d)
{
    if (core_csp_domain_is_type(*d, core_csp_dom_sparse_t)) {
        core_csp_dom_sparse_t sp = core_csp_domain_get(*d, core_csp_dom_sparse_t);
        core_free(sp.values);
    }

    *d = core_variant_empty(core_csp_domain_t);
}

int64_t
core_csp_dom_min(const core_csp_domain_t *d)
{
    switch (d->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t):
        return core_csp_domain_get(*d, core_csp_dom_interval_t).lo;
    case core_csp_domain_tag(core_csp_dom_bitset_t): {
        core_csp_dom_bitset_t bs = core_csp_domain_get(*d, core_csp_dom_bitset_t);
        return bs.base + ctz64(bs.bits);
    }
    case core_csp_domain_tag(core_csp_dom_sparse_t): {
        core_csp_dom_sparse_t sp = core_csp_domain_get(*d, core_csp_dom_sparse_t);
        return sp.count > 0 ? sp.values[0] : 0;
    }
    default:
        return 0;
    }
}

int64_t
core_csp_dom_max(const core_csp_domain_t *d)
{
    switch (d->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t):
        return core_csp_domain_get(*d, core_csp_dom_interval_t).hi;
    case core_csp_domain_tag(core_csp_dom_bitset_t): {
        core_csp_dom_bitset_t bs = core_csp_domain_get(*d, core_csp_dom_bitset_t);
        return bs.base + (63 - clz64(bs.bits));
    }
    case core_csp_domain_tag(core_csp_dom_sparse_t): {
        core_csp_dom_sparse_t sp = core_csp_domain_get(*d, core_csp_dom_sparse_t);
        return sp.count > 0 ? sp.values[sp.count - 1] : 0;
    }
    default:
        return 0;
    }
}

int64_t
core_csp_dom_size(const core_csp_domain_t *d)
{
    switch (d->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t): {
        core_csp_dom_interval_t iv = core_csp_domain_get(*d,
                                                      core_csp_dom_interval_t);
        uint64_t span = (uint64_t)iv.hi - (uint64_t)iv.lo;
        return span >= (uint64_t)INT64_MAX ? INT64_MAX
                                           : (int64_t)(span + 1);
    }
    case core_csp_domain_tag(core_csp_dom_bitset_t):
        return popcount64(core_csp_domain_get(*d, core_csp_dom_bitset_t).bits);
    case core_csp_domain_tag(core_csp_dom_sparse_t):
        return core_csp_domain_get(*d, core_csp_dom_sparse_t).count;
    default:
        return 0;
    }
}

bool
core_csp_dom_is_singleton(const core_csp_domain_t *d)
{
    switch (d->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t): {
        core_csp_dom_interval_t interval =
            core_csp_domain_get(*d, core_csp_dom_interval_t);
        return interval.lo == interval.hi;
    }
    case core_csp_domain_tag(core_csp_dom_bitset_t):
        return popcount64(
                   core_csp_domain_get(*d, core_csp_dom_bitset_t).bits) == 1;
    case core_csp_domain_tag(core_csp_dom_sparse_t):
        return core_csp_domain_get(*d, core_csp_dom_sparse_t).count == 1;
    default:
        return false;
    }
}

bool
core_csp_dom_is_empty(const core_csp_domain_t *d)
{
    return !core_csp_domain_is_set(*d)
           || (core_csp_domain_is_type(*d, core_csp_dom_bitset_t)
               && core_csp_domain_get(*d, core_csp_dom_bitset_t).bits == 0)
           || (core_csp_domain_is_type(*d, core_csp_dom_sparse_t)
               && core_csp_domain_get(*d, core_csp_dom_sparse_t).count == 0);
}

bool
core_csp_dom_contains(const core_csp_domain_t *d, int64_t val)
{
    switch (d->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t): {
        core_csp_dom_interval_t iv = core_csp_domain_get(*d,
                                                      core_csp_dom_interval_t);
        return val >= iv.lo && val <= iv.hi;
    }
    case core_csp_domain_tag(core_csp_dom_bitset_t): {
        core_csp_dom_bitset_t bs     = core_csp_domain_get(*d,
                                                    core_csp_dom_bitset_t);
        int64_t               offset = val - bs.base;

        if (offset < 0 || offset > 63) {
            return false;
        }

        return (bs.bits >> offset) & 1;
    }
    case core_csp_domain_tag(core_csp_dom_sparse_t): {
        core_csp_dom_sparse_t sp = core_csp_domain_get(*d, core_csp_dom_sparse_t);
        int32_t               lo = 0, hi = sp.count - 1;

        while (lo <= hi) {
            int32_t mid = lo + (hi - lo) / 2;

            if (sp.values[mid] == val) {
                return true;
            }

            if (sp.values[mid] < val) {
                lo = mid + 1;
            }
            else {
                hi = mid - 1;
            }
        }

        return false;
    }
    default:
        return false;
    }
}

bool
core_csp_dom_visit(const core_csp_domain_t *d,
                   core_csp_dom_visit_cb callback, void *context)
{
    if (!d || !callback) return false;
    switch (d->tag) {
    case CORE_CSP_DOMAIN_INTERVAL: {
        core_csp_dom_interval_t interval =
            core_csp_domain_get(*d, core_csp_dom_interval_t);
        for (int64_t value = interval.lo;; value++) {
            if (!callback(value, context)) return false;
            if (value == interval.hi) break;
        }
        return true;
    }
    case CORE_CSP_DOMAIN_BITSET: {
        core_csp_dom_bitset_t bitset =
            core_csp_domain_get(*d, core_csp_dom_bitset_t);
        for (uint64_t bits = bitset.bits; bits; bits &= bits - 1) {
            int64_t value = bitset.base + __builtin_ctzll(bits);
            if (!callback(value, context)) return false;
        }
        return true;
    }
    case CORE_CSP_DOMAIN_SPARSE: {
        core_csp_dom_sparse_t sparse =
            core_csp_domain_get(*d, core_csp_dom_sparse_t);
        for (int32_t i = 0; i < sparse.count; i++)
            if (!callback(sparse.values[i], context)) return false;
        return true;
    }
    default:
        return true;
    }
}

static core_csp_domain_t
interval_to_bitset(int64_t lo, int64_t hi)
{
    core_csp_dom_bitset_t bs = {
        .base = lo,
        .bits = 0,
    };

    for (int64_t v = lo; v <= hi; v++) {
        bs.bits |= (uint64_t)1 << (v - lo);
    }

    return core_csp_domain_set(core_csp_domain_t, core_csp_dom_bitset_t, bs);
}

static core_csp_domain_t
interval_to_sparse(int64_t lo, int64_t hi)
{
    int32_t               count = (int32_t)(hi - lo + 1);
    core_csp_dom_sparse_t sp     = {
            .values = core_alloc_array(int64_t, count),
            .count  = count,
            .cap    = count,
    };

    for (int32_t i = 0; i < count; i++) {
        sp.values[i] = lo + i;
    }

    return core_csp_domain_set(core_csp_domain_t, core_csp_dom_sparse_t, sp);
}

bool
core_csp_dom_intersect(core_csp_domain_t *d, const core_csp_domain_t *other)
{
    if (!core_csp_domain_is_set(*d)) {
        return false;
    }

    if (!core_csp_domain_is_set(*other)) {
        core_csp_dom_free(d);
        *d = core_variant_empty(core_csp_domain_t);
        return true;
    }

    if (core_csp_domain_is_type(*d, core_csp_dom_interval_t)
        && core_csp_domain_is_type(*other, core_csp_dom_interval_t)) {
        core_csp_dom_interval_t di = core_csp_domain_get(*d,
                                                      core_csp_dom_interval_t);
        core_csp_dom_interval_t oi = core_csp_domain_get(*other,
                                                      core_csp_dom_interval_t);
        int64_t new_lo = di.lo > oi.lo ? di.lo : oi.lo;
        int64_t new_hi = di.hi < oi.hi ? di.hi : oi.hi;

        if (new_lo > new_hi) {
            *d = core_variant_empty(core_csp_domain_t);
            return true;
        }

        bool changed = (new_lo != di.lo || new_hi != di.hi);
        *d           = core_csp_domain_set(core_csp_domain_t,
                              core_csp_dom_interval_t,
                              ((core_csp_dom_interval_t){
                                            .lo = new_lo,
                                            .hi = new_hi,
                              }));

        return changed;
    }

    if (core_csp_domain_is_type(*d, core_csp_dom_bitset_t)
        && core_csp_domain_is_type(*other, core_csp_dom_bitset_t)) {
        core_csp_dom_bitset_t db = core_csp_domain_get(*d, core_csp_dom_bitset_t);
        core_csp_dom_bitset_t ob = core_csp_domain_get(*other,
                                                    core_csp_dom_bitset_t);
        int64_t d_max      = db.base + 63;
        int64_t o_max      = ob.base + 63;
        int64_t overlap_lo = db.base > ob.base ? db.base : ob.base;
        int64_t overlap_hi = d_max < o_max ? d_max : o_max;

        if (overlap_lo > overlap_hi) {
            uint64_t old = db.bits;
            *d           = core_variant_empty(core_csp_domain_t);
            return old != 0;
        }

        uint64_t d_shifted = db.bits >> (overlap_lo - db.base);
        uint64_t o_shifted = ob.bits >> (overlap_lo - ob.base);

        int64_t  range = overlap_hi - overlap_lo + 1;
        uint64_t mask =
            range >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << range) - 1;

        uint64_t new_bits = d_shifted & o_shifted & mask;
        bool changed = (new_bits != db.bits || overlap_lo != db.base);

        if (new_bits == 0) {
            *d = core_variant_empty(core_csp_domain_t);
        }
        else {
            *d = core_csp_domain_set(core_csp_domain_t,
                                  core_csp_dom_bitset_t,
                                  ((core_csp_dom_bitset_t){
                                      .base = overlap_lo,
                                      .bits = new_bits,
                                  }));
        }

        return changed;
    }

    int64_t *tmp       = nullptr;
    int32_t  tmp_count = 0;
    int32_t  tmp_cap   = 0;

    switch (d->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t): {
        core_csp_dom_interval_t iv = core_csp_domain_get(*d,
                                                      core_csp_dom_interval_t);
        for (int64_t v = iv.lo; v <= iv.hi; v++) {
            if (core_csp_dom_contains(other, v)) {
                if (tmp_count >= tmp_cap) {
                    int32_t  new_cap = tmp_cap ? tmp_cap * 2 : 16;
                    int64_t *new_tmp = core_alloc_array(int64_t, new_cap);
                    if (tmp_count > 0) {
                        memcpy(new_tmp, tmp, tmp_count * sizeof(int64_t));
                    }
                    core_free(tmp);
                    tmp     = new_tmp;
                    tmp_cap = new_cap;
                }
                tmp[tmp_count++] = v;
            }
        }
        break;
    }
    case core_csp_domain_tag(core_csp_dom_bitset_t): {
        core_csp_dom_bitset_t bs = core_csp_domain_get(*d, core_csp_dom_bitset_t);
        for (uint64_t bits = bs.bits; bits;) {
            int     bit = ctz64(bits);
            int64_t v   = bs.base + bit;

            if (core_csp_dom_contains(other, v)) {
                if (tmp_count >= tmp_cap) {
                    int32_t  new_cap = tmp_cap ? tmp_cap * 2 : 16;
                    int64_t *new_tmp = core_alloc_array(int64_t, new_cap);
                    if (tmp_count > 0) {
                        memcpy(new_tmp, tmp, tmp_count * sizeof(int64_t));
                    }
                    core_free(tmp);
                    tmp     = new_tmp;
                    tmp_cap = new_cap;
                }
                tmp[tmp_count++] = v;
            }

            bits &= bits - 1;
        }
        break;
    }
    case core_csp_domain_tag(core_csp_dom_sparse_t): {
        core_csp_dom_sparse_t sp = core_csp_domain_get(*d, core_csp_dom_sparse_t);
        for (int32_t i = 0; i < sp.count; i++) {
            if (core_csp_dom_contains(other, sp.values[i])) {
                if (tmp_count >= tmp_cap) {
                    int32_t  new_cap = tmp_cap ? tmp_cap * 2 : 16;
                    int64_t *new_tmp = core_alloc_array(int64_t, new_cap);
                    if (tmp_count > 0) {
                        memcpy(new_tmp, tmp, tmp_count * sizeof(int64_t));
                    }
                    core_free(tmp);
                    tmp     = new_tmp;
                    tmp_cap = new_cap;
                }
                tmp[tmp_count++] = sp.values[i];
            }
        }
        break;
    }
    default:
        break;
    }

    int64_t old_size = core_csp_dom_size(d);
    core_csp_dom_free(d);

    if (tmp_count == 0) {
        core_free(tmp);
        *d = core_variant_empty(core_csp_domain_t);
        return old_size > 0;
    }

    *d = core_csp_dom_from_values(tmp, tmp_count);
    core_free(tmp);

    return tmp_count != old_size;
}

bool
core_csp_dom_remove_value(core_csp_domain_t *d, int64_t val)
{
    if (!core_csp_dom_contains(d, val)) {
        return false;
    }

    switch (d->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t): {
        core_csp_dom_interval_t iv = core_csp_domain_get(*d,
                                                      core_csp_dom_interval_t);
        if (val == iv.lo && val == iv.hi) {
            *d = core_variant_empty(core_csp_domain_t);
            return true;
        }

        if (val == iv.lo) {
            iv.lo++;
            *d = core_csp_domain_set(core_csp_domain_t,
                                  core_csp_dom_interval_t,
                                  iv);
            return true;
        }

        if (val == iv.hi) {
            iv.hi--;
            *d = core_csp_domain_set(core_csp_domain_t,
                                  core_csp_dom_interval_t,
                                  iv);
            return true;
        }

        int64_t range = iv.hi - iv.lo + 1;

        if (range <= 64) {
            *d = interval_to_bitset(iv.lo, iv.hi);
            core_csp_dom_bitset_t bs = core_csp_domain_get(*d,
                                                        core_csp_dom_bitset_t);
            bs.bits &= ~((uint64_t)1 << (val - bs.base));
            *d = core_csp_domain_set(core_csp_domain_t, core_csp_dom_bitset_t, bs);
            return true;
        }

        *d = interval_to_sparse(iv.lo, iv.hi);
    }
        [[fallthrough]];
    case core_csp_domain_tag(core_csp_dom_sparse_t): {
        core_csp_dom_sparse_t sp     = core_csp_domain_get(*d,
                                                    core_csp_dom_sparse_t);
        int32_t               lo_idx = 0, hi_idx = sp.count - 1;

        while (lo_idx <= hi_idx) {
            int32_t mid = lo_idx + (hi_idx - lo_idx) / 2;

            if (sp.values[mid] == val) {
                memmove(&sp.values[mid],
                        &sp.values[mid + 1],
                        (sp.count - mid - 1) * sizeof(int64_t));
                sp.count--;

                if (sp.count == 0) {
                    *d = core_variant_empty(core_csp_domain_t);
                }
                else {
                    *d = core_csp_domain_set(core_csp_domain_t,
                                          core_csp_dom_sparse_t,
                                          sp);
                }

                return true;
            }

            if (sp.values[mid] < val) {
                lo_idx = mid + 1;
            }
            else {
                hi_idx = mid - 1;
            }
        }

        return false;
    }
    case core_csp_domain_tag(core_csp_dom_bitset_t): {
        core_csp_dom_bitset_t bs     = core_csp_domain_get(*d,
                                                    core_csp_dom_bitset_t);
        int64_t               offset = val - bs.base;
        bs.bits &= ~((uint64_t)1 << offset);

        if (bs.bits == 0) {
            *d = core_variant_empty(core_csp_domain_t);
        }
        else {
            *d = core_csp_domain_set(core_csp_domain_t, core_csp_dom_bitset_t, bs);
        }

        return true;
    }
    default:
        return false;
    }
}

bool
core_csp_dom_restrict_min(core_csp_domain_t *d, int64_t new_min)
{
    if (core_csp_dom_is_empty(d)) {
        return false;
    }

    int64_t cur_min = core_csp_dom_min(d);

    if (new_min <= cur_min) {
        return false;
    }

    switch (d->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t): {
        core_csp_dom_interval_t iv = core_csp_domain_get(*d,
                                                      core_csp_dom_interval_t);
        if (new_min > iv.hi) {
            *d = core_variant_empty(core_csp_domain_t);
            return true;
        }

        iv.lo = new_min;
        *d    = core_csp_domain_set(core_csp_domain_t, core_csp_dom_interval_t, iv);
        return true;
    }
    case core_csp_domain_tag(core_csp_dom_bitset_t): {
        core_csp_dom_bitset_t bs     = core_csp_domain_get(*d,
                                                    core_csp_dom_bitset_t);
        int64_t               offset = new_min - bs.base;

        if (offset > 63) {
            *d = core_variant_empty(core_csp_domain_t);
            return true;
        }

        if (offset > 0) {
            uint64_t mask = ~(((uint64_t)1 << offset) - 1);
            uint64_t old  = bs.bits;
            bs.bits &= mask;

            if (bs.bits == 0) {
                *d = core_variant_empty(core_csp_domain_t);
            }
            else {
                *d = core_csp_domain_set(core_csp_domain_t,
                                      core_csp_dom_bitset_t,
                                      bs);
            }

            return bs.bits != old;
        }

        return false;
    }
    case core_csp_domain_tag(core_csp_dom_sparse_t): {
        core_csp_dom_sparse_t sp = core_csp_domain_get(*d, core_csp_dom_sparse_t);
        int32_t               i  = 0;

        while (i < sp.count && sp.values[i] < new_min) {
            i++;
        }

        if (i == sp.count) {
            *d = core_variant_empty(core_csp_domain_t);
            return true;
        }

        if (i > 0) {
            memmove(sp.values,
                    &sp.values[i],
                    (sp.count - i) * sizeof(int64_t));
            sp.count -= i;
            *d = core_csp_domain_set(core_csp_domain_t,
                                  core_csp_dom_sparse_t,
                                  sp);
            return true;
        }

        return false;
    }
    default:
        return false;
    }
}

bool
core_csp_dom_restrict_max(core_csp_domain_t *d, int64_t new_max)
{
    if (core_csp_dom_is_empty(d)) {
        return false;
    }

    int64_t cur_max = core_csp_dom_max(d);

    if (new_max >= cur_max) {
        return false;
    }

    switch (d->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t): {
        core_csp_dom_interval_t iv = core_csp_domain_get(*d,
                                                      core_csp_dom_interval_t);
        if (new_max < iv.lo) {
            *d = core_variant_empty(core_csp_domain_t);
            return true;
        }

        iv.hi = new_max;
        *d    = core_csp_domain_set(core_csp_domain_t, core_csp_dom_interval_t, iv);
        return true;
    }
    case core_csp_domain_tag(core_csp_dom_bitset_t): {
        core_csp_dom_bitset_t bs     = core_csp_domain_get(*d,
                                                    core_csp_dom_bitset_t);
        int64_t               offset = new_max - bs.base;

        if (offset < 0) {
            *d = core_variant_empty(core_csp_domain_t);
            return true;
        }

        uint64_t mask = ((uint64_t)1 << (offset + 1)) - 1;
        uint64_t old  = bs.bits;
        bs.bits &= mask;

        if (bs.bits == 0) {
            *d = core_variant_empty(core_csp_domain_t);
        }
        else {
            *d = core_csp_domain_set(core_csp_domain_t, core_csp_dom_bitset_t, bs);
        }

        return bs.bits != old;
    }
    case core_csp_domain_tag(core_csp_dom_sparse_t): {
        core_csp_dom_sparse_t sp = core_csp_domain_get(*d, core_csp_dom_sparse_t);
        int32_t               i  = sp.count - 1;

        while (i >= 0 && sp.values[i] > new_max) {
            i--;
        }

        int32_t new_count = i + 1;

        if (new_count == 0) {
            *d = core_variant_empty(core_csp_domain_t);
            return true;
        }

        if (new_count < sp.count) {
            sp.count = new_count;
            *d = core_csp_domain_set(core_csp_domain_t, core_csp_dom_sparse_t, sp);
            return true;
        }

        return false;
    }
    default:
        return false;
    }
}
