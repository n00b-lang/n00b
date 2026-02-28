/*
 * One-dimensional constraint-based layout engine.
 *
 * Ported from ~/n00b-old/src/text/layout.nc into a flat-array API
 * (no tree dependency).  The algorithm is identical: min/max/pref
 * constraints with flex growth and priority-based cropping.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "display/layout.h"

// -------------------------------------------------------------------
// Internal: sort helpers
// -------------------------------------------------------------------

// Index wrapper for sorting without modifying the results array order.
typedef struct {
    n00b_layout_result_t *result;
    int64_t               flex_multiple;
    int64_t               priority;
    int64_t               order; // Original index
} layout_item_t;

static int
ascending_size(const void *a, const void *b)
{
    const layout_item_t *ia = a;
    const layout_item_t *ib = b;

    if (ia->result->size != ib->result->size) {
        return ia->result->size < ib->result->size ? -1 : 1;
    }
    if (ia->result->computed_max != ib->result->computed_max) {
        return ia->result->computed_max < ib->result->computed_max ? -1 : 1;
    }
    return 0;
}

static int
descending_size(const void *a, const void *b)
{
    const layout_item_t *ia = a;
    const layout_item_t *ib = b;

    if (ia->result->size != ib->result->size) {
        return ia->result->size > ib->result->size ? -1 : 1;
    }
    if (ia->result->computed_min != ib->result->computed_min) {
        return ia->result->computed_min > ib->result->computed_min ? -1 : 1;
    }
    return 0;
}

static int
ascending_priority(const void *a, const void *b)
{
    const layout_item_t *ia = a;
    const layout_item_t *ib = b;

    if (ia->priority != ib->priority) {
        return ia->priority < ib->priority ? -1 : 1;
    }
    // Equal priority: higher original order sorted first (we steal from back).
    return ia->order > ib->order ? -1 : 1;
}

// -------------------------------------------------------------------
// Internal: expand items toward their max (smallest-first)
// -------------------------------------------------------------------

static int64_t
expand_to_max(layout_item_t *sorted, int64_t count, int64_t available)
{
    // sorted is in ascending size order.
    // Expand the smallest group toward the next group's size (or max).
    int64_t i = 0;

    while (i < count && available > 0) {
        // Find the group of items that share the current smallest size.
        int64_t group_start = i;
        int64_t cur_size    = sorted[i].result->size;

        while (i < count && sorted[i].result->size == cur_size) {
            i++;
        }
        int64_t group_count = i - group_start;

        // Target: next group's size, or our own max, whichever is smaller.
        int64_t target;
        if (i < count) {
            target = sorted[i].result->size;
        }
        else {
            target = INT64_MAX;
        }

        // Clamp each item to its own max.
        for (int64_t j = group_start; j < group_start + group_count; j++) {
            int64_t item_max = sorted[j].result->computed_max;
            if (item_max > 0 && item_max < target) {
                // This item can't grow as much; handle individually.
            }
        }

        // Grow uniformly toward target.
        for (int64_t j = group_start; j < group_start + group_count && available > 0; j++) {
            int64_t item_max = sorted[j].result->computed_max;
            int64_t limit    = target;
            if (item_max > 0 && item_max < limit) {
                limit = item_max;
            }
            int64_t grow = limit - sorted[j].result->size;
            if (grow > 0) {
                if (grow > available) {
                    grow = available;
                }
                sorted[j].result->size += grow;
                available -= grow;
            }
        }

        // Reset to re-sort and retry with the updated state.
        if (available > 0) {
            qsort(sorted, count, sizeof(layout_item_t), ascending_size);
            i = 0;
            // Skip items already at their max.
            while (i < count) {
                int64_t mx = sorted[i].result->computed_max;
                if (mx > 0 && sorted[i].result->size >= mx) {
                    i++;
                }
                else {
                    break;
                }
            }
            if (i >= count) {
                break;
            }
        }
    }

    return available;
}

// -------------------------------------------------------------------
// Internal: flex growth
// -------------------------------------------------------------------

static int64_t
apply_flex_growth(layout_item_t *items, int64_t n, int64_t available)
{
    int64_t total_flex = 0;

    for (int64_t i = 0; i < n; i++) {
        // Only flex items without a max constraint (or already at max) grow.
        if (items[i].flex_multiple > 0 && items[i].result->computed_max == 0) {
            total_flex += items[i].flex_multiple;
        }
    }

    if (total_flex == 0 || available <= 0) {
        return available;
    }

    int64_t flex_unit  = available / total_flex;
    int64_t remainder  = available % total_flex;
    int64_t rem_given  = 0;
    int64_t distributed = 0;

    for (int64_t i = 0; i < n; i++) {
        if (items[i].flex_multiple > 0 && items[i].result->computed_max == 0) {
            int64_t add = flex_unit * items[i].flex_multiple;
            if (rem_given < remainder) {
                int64_t extra = n00b_min(items[i].flex_multiple,
                                          remainder - rem_given);
                add += extra;
                rem_given += extra;
            }
            items[i].result->size += add;
            distributed += add;
        }
    }

    return available - distributed;
}

// -------------------------------------------------------------------
// Internal: shrink to fit
// -------------------------------------------------------------------

static int64_t
shrink_to_fit(layout_item_t *sorted, int64_t count, int64_t deficit)
{
    // deficit is positive (amount we need to remove).
    // sorted is in descending size order.
    // Shrink largest items toward the next group's size, respecting min.

    int64_t i = 0;

    while (i < count && deficit > 0) {
        int64_t group_start = i;
        int64_t cur_size    = sorted[i].result->size;

        while (i < count && sorted[i].result->size == cur_size) {
            i++;
        }
        int64_t group_count = i - group_start;

        // Target: next group's size, or our min, whichever is larger.
        int64_t target = 0;
        if (i < count) {
            target = sorted[i].result->size;
        }

        for (int64_t j = group_start; j < group_start + group_count && deficit > 0; j++) {
            int64_t item_min = sorted[j].result->computed_min;
            int64_t limit    = n00b_max(target, item_min);
            int64_t shrink   = sorted[j].result->size - limit;
            if (shrink > 0) {
                if (shrink > deficit) {
                    shrink = deficit;
                }
                sorted[j].result->size -= shrink;
                deficit -= shrink;
            }
        }

        if (deficit > 0) {
            qsort(sorted, count, sizeof(layout_item_t), descending_size);
            i = 0;
            // Skip items already at their min.
            while (i < count) {
                if (sorted[i].result->size <= sorted[i].result->computed_min) {
                    i++;
                }
                else {
                    break;
                }
            }
            if (i >= count) {
                break;
            }
        }
    }

    return deficit;
}

// -------------------------------------------------------------------
// Internal: force-crop by priority
// -------------------------------------------------------------------

static void
crop_by_priority(layout_item_t *sorted, int64_t count, int64_t deficit)
{
    // sorted in ascending priority order.
    // Lowest priority items lose allocation first.
    // Within an equal-priority group, distribute the deficit evenly
    // so no single item is zeroed while siblings keep their full size.

    int64_t i = 0;

    while (i < count && deficit > 0) {
        // Find the group of items that share the current priority.
        int64_t group_start = i;
        int64_t cur_pri     = sorted[i].priority;

        while (i < count && sorted[i].priority == cur_pri) {
            i++;
        }

        int64_t group_count = i - group_start;

        // Compute total size in this priority group.
        int64_t group_total = 0;
        for (int64_t j = group_start; j < group_start + group_count; j++) {
            group_total += sorted[j].result->size;
        }

        if (group_total <= deficit) {
            // Entire group is consumed.
            for (int64_t j = group_start; j < group_start + group_count; j++) {
                deficit -= sorted[j].result->size;
                sorted[j].result->size = 0;
            }
        }
        else {
            // Distribute deficit evenly across this group, giving each
            // item at least 1 cell when possible.
            int64_t per_item = deficit / group_count;
            int64_t rem      = deficit % group_count;

            for (int64_t j = group_start; j < group_start + group_count; j++) {
                int64_t take = per_item + (j - group_start < rem ? 1 : 0);
                if (take > sorted[j].result->size) {
                    take = sorted[j].result->size;
                }
                sorted[j].result->size -= take;
            }
            deficit = 0;
        }
    }
}

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

void
n00b_layout_calculate(n00b_array_t(n00b_layout_t)        items_arr,
                       n00b_array_t(n00b_layout_result_t) results_arr,
                       int64_t                            available)
{
    n00b_isize_t              n       = (n00b_isize_t)items_arr.len;
    const n00b_layout_t      *items   = items_arr.data;
    n00b_layout_result_t     *results = results_arr.data;

    if (n == 0) {
        return;
    }

    // Stack-allocate work array for small n; heap for large.
    layout_item_t  stack_buf[32];
    layout_item_t *work;

    if (n <= 32) {
        work = stack_buf;
    }
    else {
        work = n00b_alloc_array(layout_item_t, n);
    }

    // Phase 1: Resolve dimensions and set initial sizes.
    int64_t total = 0;

    for (n00b_isize_t i = 0; i < n; i++) {
        results[i].computed_min  = n00b_layout_resolve_dim(&items[i].min, available);
        results[i].computed_max  = n00b_layout_resolve_dim(&items[i].max, available);
        results[i].computed_pref = n00b_layout_resolve_dim(&items[i].pref, available);

        // Ensure min <= pref <= max when all are set.
        if (results[i].computed_max > 0
            && results[i].computed_max < results[i].computed_min) {
            results[i].computed_max = results[i].computed_min;
        }
        if (results[i].computed_pref < results[i].computed_min) {
            results[i].computed_pref = results[i].computed_min;
        }
        if (results[i].computed_max > 0
            && results[i].computed_pref > results[i].computed_max) {
            results[i].computed_pref = results[i].computed_max;
        }

        // Initial size: max(min, pref).
        results[i].size = n00b_max(results[i].computed_min, results[i].computed_pref);
        total += results[i].size;

        work[i].result        = &results[i];
        work[i].flex_multiple = items[i].flex_multiple;
        work[i].priority      = items[i].priority;
        work[i].order         = i;
    }

    int64_t leftover = available - total;

    // Phase 2: If space remains, grow toward max (smallest-first).
    if (leftover > 0) {
        // Build list of items with a max > current size.
        int64_t        grow_count = 0;
        layout_item_t  grow_stack[32];
        layout_item_t *grow_items = (n <= 32) ? grow_stack : work;

        for (n00b_isize_t i = 0; i < n; i++) {
            if (results[i].computed_max > 0
                && results[i].computed_max > results[i].size) {
                grow_items[grow_count++] = work[i];
            }
        }

        if (grow_count > 0) {
            qsort(grow_items, grow_count, sizeof(layout_item_t), ascending_size);
            leftover = expand_to_max(grow_items, grow_count, leftover);
        }
    }

    // Phase 3: Distribute remaining space via flex.
    if (leftover > 0) {
        leftover = apply_flex_growth(work, n, leftover);
    }

    // Phase 4: If over-committed, shrink to fit.
    if (leftover < 0) {
        int64_t deficit = -leftover;

        // Build list of items that can shrink.
        int64_t        shrink_count = 0;
        layout_item_t  shrink_stack[32];
        layout_item_t *shrink_items = (n <= 32) ? shrink_stack : work;

        for (n00b_isize_t i = 0; i < n; i++) {
            if (results[i].size > results[i].computed_min) {
                shrink_items[shrink_count++] = work[i];
            }
        }

        if (shrink_count > 0) {
            qsort(shrink_items, shrink_count, sizeof(layout_item_t), descending_size);
            deficit = shrink_to_fit(shrink_items, shrink_count, deficit);
        }

        // Phase 5: Force-crop by priority if still over.
        if (deficit > 0) {
            qsort(work, n, sizeof(layout_item_t), ascending_priority);
            crop_by_priority(work, n, deficit);
        }
    }

    if (n > 32) {
        n00b_free(work);
    }
}
