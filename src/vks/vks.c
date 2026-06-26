#include "vks/vks.h"
#include "core/atomic.h"
#include "core/epoch.h"

// ============================================================================
// Open / mutation hooks
// ============================================================================

/*
 * Rehydrate the store from its backend right after construction.
 *
 * If a backend with a load op is attached, ask it to load the durable image
 * into the store. With no backend (or no load op) this is a no-op, leaving the
 * freshly-built empty dict in place. A load failure is intentionally swallowed:
 * the durability contract (D-012) treats a missing or unusable snapshot as
 * "start empty", so callers always get a usable store.
 */
void
_n00b_vks_post_open(_n00b_vks_store_internal_t *s)
{
    if (s == nullptr || s->backend == nullptr) {
        return;
    }

    const n00b_vks_backend_ops_t *ops = s->backend->ops;
    if (ops == nullptr || ops->load == nullptr) {
        return;
    }

    // Result is deliberately ignored: load reports "nothing usable" via
    // ok(false) and even a hard error must not prevent the store from opening.
    (void)ops->load(s->backend->ctx, s);
}

/*
 * Bump the dirty counter after a mutation, snapshotting at threshold.
 *
 * Counts every mutation. When a backend is attached and dirty_max is set,
 * crossing the threshold fires one full snapshot and resets dirty to 0. With
 * backend==nullptr this only counts (and the tests observe the count).
 */
void
_n00b_vks_after_mutation(_n00b_vks_store_internal_t *s)
{
    if (s == nullptr) {
        return;
    }

    s->dirty++;

    if (s->backend == nullptr || s->dirty_max == 0
        || s->dirty < s->dirty_max) {
        return;
    }

    const n00b_vks_backend_ops_t *ops = s->backend->ops;
    if (ops != nullptr && ops->snapshot != nullptr) {
        // A snapshot failure leaves dirty as-is so the next mutation retries at
        // the threshold rather than silently dropping the dirty window.
        n00b_result_t(bool) r = ops->snapshot(s->backend->ctx, s);
        if (n00b_result_is_ok(r)) {
            s->dirty = 0;
        }
    }
}

// ============================================================================
// Key enumeration
// ============================================================================

/*
 * Return all keys currently in the store as a list of key pointers.
 *
 * Walks the embedded dict's type-erased store directly, mirroring the iteration
 * shape of n00b_dict_foreach (skip empty + DELETED buckets). Phase 1 supports
 * pointer-keyed stores: each live key slot holds a single pointer, which is
 * pushed into the result list. On success the result holds exactly the live key
 * count.
 */
n00b_result_t(n00b_list_t *)
n00b_vks_keys(void *store)
{
    if (store == nullptr) {
        return n00b_result_err(n00b_list_t *, N00B_VKS_ERR_NULL_ARG);
    }

    _n00b_vks_store_internal_t *s = (_n00b_vks_store_internal_t *)store;

    if (s->mem == nullptr) {
        return n00b_result_err(n00b_list_t *, N00B_VKS_ERR_NULL_ARG);
    }

    _n00b_dict_internal_t *d = (_n00b_dict_internal_t *)s->mem;

    // Inside the epoch window we MUST NOT allocate or push onto a list (those
    // can block / run the collector). So we only COLLECT live key pointers into
    // a pre-sized local buffer here, then yield, then build the result list and
    // push the collected keys OUTSIDE the window.
    //
    // Size the buffer from the dict's live entry count; that is an exact upper
    // bound on the keys we can collect (slots can only be removed, not added,
    // while we hold the reservation).
    n00b_size_t cap = n00b_dict_internal_len(d);

    void   **collected = nullptr;
    uint32_t n_keys    = 0;

    if (cap > 0) {
        collected = n00b_alloc_array(void *, cap, .allocator = s->allocator);
    }

    n00b_epoch_acquire();

    __n00b_internal_type_erased_store_t *ds
        = (__n00b_internal_type_erased_store_t *)n00b_atomic_load(&d->store);

    if (ds != nullptr && collected != nullptr) {
        void **keys = (void **)ds->keys;
        for (uint32_t i = 0; i <= ds->last_slot && n_keys < cap; i++) {
            n00b_dict_bucket_t *b = &ds->buckets[i];
            if (b->hv != (n00b_uint128_t)0
                && !(n00b_atomic_load(&b->flags) & N00B_HT_FLAG_DELETED)) {
                collected[n_keys++] = keys[i];
            }
        }
    }

    n00b_epoch_yield();

    // The result list holds raw key pointers (n00b_string_t * in the Phase 1
    // case). It is layout-identical to n00b_list_t(K *) for any pointer K, so
    // the caller can consume it through its typed list view.
    n00b_list_t(void *) *out = n00b_alloc(n00b_list_t(void *),
                                          .allocator = s->allocator);
    *out = n00b_list_new(void *, .allocator = s->allocator);

    for (uint32_t i = 0; i < n_keys; i++) {
        n00b_list_push(*out, collected[i]);
    }

    return n00b_result_ok(n00b_list_t *, (n00b_list_t *)out);
}

// ============================================================================
// Flush / close
// ============================================================================

/*
 * Flush buffered mutations to the durability backend.
 *
 * With no backend this is a no-op that resets the dirty counter and returns
 * ok(true). With a backend it forces a full snapshot and resets dirty to 0 on
 * success.
 */
n00b_result_t(bool)
n00b_vks_flush(void *store)
{
    if (store == nullptr) {
        return n00b_result_err(bool, N00B_VKS_ERR_NULL_ARG);
    }

    _n00b_vks_store_internal_t *s = (_n00b_vks_store_internal_t *)store;

    if (s->backend == nullptr) {
        // No durability target — nothing to persist. Reset dirty so callers can
        // treat a clean flush uniformly with or without a backend.
        s->dirty = 0;
        return n00b_result_ok(bool, true);
    }

    const n00b_vks_backend_ops_t *ops = s->backend->ops;
    if (ops == nullptr || ops->snapshot == nullptr) {
        return n00b_result_err(bool, N00B_VKS_ERR_NOT_SUPPORTED);
    }

    n00b_result_t(bool) r = ops->snapshot(s->backend->ctx, s);
    if (n00b_result_is_err(r)) {
        return r;
    }

    s->dirty = 0;
    return n00b_result_ok(bool, true);
}

/*
 * Flush and release the store's backend (if any).
 *
 * Flushes (a final snapshot when a backend is attached, otherwise a no-op) and
 * tears down the backend. Backing storage is GC-managed, so the store struct
 * itself is not freed here.
 */
void
n00b_vks_close(void *store)
{
    if (store == nullptr) {
        return;
    }

    (void)n00b_vks_flush(store);

    _n00b_vks_store_internal_t *s = (_n00b_vks_store_internal_t *)store;

    if (s->backend != nullptr) {
        n00b_vks_backend_cleanup(s->backend);
        s->backend = nullptr;
    }
}
