#include "text/unicode/query.h"
#include "text/unicode/properties.h"
#include "text/unicode/encoding.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include <string.h>

// ===========================================================================
// Internal filter predicates
// ===========================================================================

static bool
_pred_gc(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_general_category(cp) == (n00b_unicode_gc_t)(uintptr_t)ctx;
}

static bool
_pred_script(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_script(cp) == (n00b_unicode_script_t)(uintptr_t)ctx;
}

static bool
_pred_bidi(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_bidi_class(cp) == (n00b_unicode_bidi_class_t)(uintptr_t)ctx;
}

static bool
_pred_property(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_has_property(cp, (n00b_unicode_property_t)(uintptr_t)ctx);
}

typedef struct {
    n00b_codepoint_t lo;
    n00b_codepoint_t hi;
} _range_ctx_t;

static bool
_pred_range(n00b_codepoint_t cp, void *ctx)
{
    _range_ctx_t *r = (_range_ctx_t *)ctx;
    return cp >= r->lo && cp <= r->hi;
}

static bool
_pred_block(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_block(cp) == (n00b_unicode_block_t)(uintptr_t)ctx;
}

static bool
_pred_eaw(n00b_codepoint_t cp, void *ctx)
{
    return n00b_unicode_east_asian_width(cp) == (n00b_unicode_eaw_t)(uintptr_t)ctx;
}

// ===========================================================================
// Filter constructors
// ===========================================================================

n00b_cp_filter_t
n00b_filter_gc(n00b_unicode_gc_t gc)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_gc,
        .ctx       = (void *)(uintptr_t)gc,
    };
}

n00b_cp_filter_t
n00b_filter_script(n00b_unicode_script_t script)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_script,
        .ctx       = (void *)(uintptr_t)script,
    };
}

n00b_cp_filter_t
n00b_filter_bidi(n00b_unicode_bidi_class_t bidi)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_bidi,
        .ctx       = (void *)(uintptr_t)bidi,
    };
}

n00b_cp_filter_t
n00b_filter_property(n00b_unicode_property_t prop)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_property,
        .ctx       = (void *)(uintptr_t)prop,
    };
}

n00b_cp_filter_t
n00b_filter_range(n00b_codepoint_t lo, n00b_codepoint_t hi)
{
    static _range_ctx_t ranges[64];
    static int          next_range = 0;

    int idx        = next_range++ % 64;
    ranges[idx].lo = lo;
    ranges[idx].hi = hi;

    return (n00b_cp_filter_t){
        .predicate = _pred_range,
        .ctx       = &ranges[idx],
    };
}

n00b_cp_filter_t
n00b_filter_block(n00b_unicode_block_t block)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_block,
        .ctx       = (void *)(uintptr_t)block,
    };
}

n00b_cp_filter_t
n00b_filter_eaw(n00b_unicode_eaw_t eaw)
{
    return (n00b_cp_filter_t){
        .predicate = _pred_eaw,
        .ctx       = (void *)(uintptr_t)eaw,
    };
}

// ===========================================================================
// Query implementation
// ===========================================================================

n00b_array_t(n00b_codepoint_t)
n00b_cp_query_n(const n00b_cp_filter_t *filters, int nfilters) _kargs
{
    n00b_codepoint_t  range_start = 0;
    n00b_codepoint_t  range_end   = 0x10FFFF;
    size_t            max_results = 0;
    n00b_allocator_t *allocator   = nullptr;
}
{
    n00b_ensure_allocator(allocator);

    size_t            cap     = 256;
    size_t            count   = 0;
    n00b_codepoint_t *results = n00b_alloc_array_with_opts(n00b_codepoint_t, cap, &(n00b_alloc_opts_t){.allocator = allocator});

    for (n00b_codepoint_t cp = range_start; cp <= range_end; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue;
        }

        // AND: all filters must match.
        bool match = true;
        for (int i = 0; i < nfilters; i++) {
            if (!filters[i].predicate(cp, filters[i].ctx)) {
                match = false;
                break;
            }
        }

        if (match) {
            if (count >= cap) {
                size_t            new_cap = cap * 2;
                n00b_codepoint_t *new_results
                    = n00b_alloc_array_with_opts(n00b_codepoint_t, new_cap, &(n00b_alloc_opts_t){.allocator = allocator});
                memcpy(new_results, results, count * sizeof(n00b_codepoint_t));
                n00b_free(results);
                results = new_results;
                cap     = new_cap;
            }
            results[count++] = cp;

            if (max_results > 0 && count >= max_results) {
                break;
            }
        }
    }

    n00b_array_t(n00b_codepoint_t) result = n00b_array_checked_ptr(n00b_codepoint_t,
                                                                    cap, results);
    result.len = count;
    return result;
}

n00b_array_t(n00b_codepoint_t)
n00b_cp_query_any_n(const n00b_cp_filter_t *filters, int nfilters) _kargs
{
    n00b_codepoint_t  range_start = 0;
    n00b_codepoint_t  range_end   = 0x10FFFF;
    size_t            max_results = 0;
    n00b_allocator_t *allocator   = nullptr;
}
{
    n00b_ensure_allocator(allocator);

    size_t            cap     = 256;
    size_t            count   = 0;
    n00b_codepoint_t *results = n00b_alloc_array_with_opts(n00b_codepoint_t, cap, &(n00b_alloc_opts_t){.allocator = allocator});

    for (n00b_codepoint_t cp = range_start; cp <= range_end; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue;
        }

        // OR: any filter matches.
        bool match = false;
        for (int i = 0; i < nfilters; i++) {
            if (filters[i].predicate(cp, filters[i].ctx)) {
                match = true;
                break;
            }
        }

        if (match) {
            if (count >= cap) {
                size_t            new_cap = cap * 2;
                n00b_codepoint_t *new_results
                    = n00b_alloc_array_with_opts(n00b_codepoint_t, new_cap, &(n00b_alloc_opts_t){.allocator = allocator});
                memcpy(new_results, results, count * sizeof(n00b_codepoint_t));
                n00b_free(results);
                results = new_results;
                cap     = new_cap;
            }
            results[count++] = cp;

            if (max_results > 0 && count >= max_results) {
                break;
            }
        }
    }

    n00b_array_t(n00b_codepoint_t) result = n00b_array_checked_ptr(n00b_codepoint_t,
                                                                    cap, results);
    result.len = count;
    return result;
}

// ===========================================================================
// Name lookup (stub - requires name table from generator)
// ===========================================================================

n00b_option_t(const char *)
n00b_unicode_cp_name(n00b_codepoint_t cp)
{
    (void)cp;
    // TODO: Implement when gen_names.c is generated by gen_tables.py.
    return n00b_option_none(const char *);
}

n00b_option_t(n00b_codepoint_t) n00b_unicode_cp_from_name(const char *name)
{
    (void)name;
    // TODO: Implement when gen_names.c is generated by gen_tables.py.
    return n00b_option_none(n00b_codepoint_t);
}
