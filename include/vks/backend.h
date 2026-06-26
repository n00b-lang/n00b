/**
 * @file backend.h
 * @brief Durability backend vtable and instance types for VKS.
 *
 * A VKS store EMBEDS an in-memory typed dict and (in Phase 2) mirrors
 * mutations to a pluggable durability backend.  Each backend implements
 * @c n00b_vks_backend_ops_t and is instantiated as an
 * @c n00b_vks_backend_t.
 *
 * All operations receive an opaque @c ctx pointer returned by @c init().
 *
 * @note Phase 1 only DECLARES these types so the store struct can hold a
 *       @c n00b_vks_backend_t* field.  No concrete backend exists yet and the
 *       vtable is never invoked (the store's @c backend field stays nullptr).
 */
#pragma once

#include "vks/types.h"

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct n00b_vks_backend     n00b_vks_backend_t;
typedef struct n00b_vks_backend_ops n00b_vks_backend_ops_t;

// ============================================================================
// Backend operations vtable
// ============================================================================

/**
 * @brief Vtable for VKS durability backend operations.
 *
 * D-012: the durability model is snapshot-only — there is no per-key
 * write-through.  A backend rehydrates the whole store on @c load and persists
 * the whole store on @c snapshot.  The store marshals its embedded dict as a
 * single object graph; backends move opaque bytes, not typed entries.  All data
 * operations return @c n00b_result_t and use VKS error codes.
 */
struct n00b_vks_backend_ops {

    /** @brief Human-readable backend name (e.g. "snapshot-vfs"). */
    n00b_string_t *(*name)(void);

    /**
     * @brief Initialize backend-specific context.
     * @param be  The backend instance (allocator is set).
     * @return    Opaque context pointer passed to all operations.
     */
    void *(*init)(n00b_vks_backend_t *be);

    /** @brief Tear down backend context and release resources. */
    void (*cleanup)(void *ctx);

    // ── Durability operations ──────────────────────────────────────

    /**
     * @brief Rehydrate @p store from durable storage on open.
     * @param ctx    Backend context from @c init.
     * @param store  Opaque pointer to the owning VKS store.
     *
     * On success returns ok(true) if a snapshot was loaded, ok(false) when
     * there was nothing to load (or the durable image was unusable) — in which
     * case the store's existing empty dict is left intact.
     */
    n00b_result_t(bool) (*load)(void *ctx, void *store);

    /**
     * @brief Persist a full snapshot of @p store to durable storage.
     * @param ctx    Backend context from @c init.
     * @param store  Opaque pointer to the owning VKS store.
     */
    n00b_result_t(bool) (*snapshot)(void *ctx, void *store);
};

// ============================================================================
// Backend instance
// ============================================================================

/**
 * @brief A configured VKS durability backend.
 *
 * Created by a backend-specific constructor (Phase 2).  The @c ctx is
 * populated by calling @c ops->init().
 */
struct n00b_vks_backend {
    const n00b_vks_backend_ops_t *ops;
    void                         *ctx;
    n00b_allocator_t             *allocator;
};

// ============================================================================
// Backend lifecycle helpers
// ============================================================================

/**
 * @brief Initialize a backend: call ops->init and store the context.
 * @pre  be->ops is set.
 * @post be->ctx is populated.
 */
extern n00b_result_t(bool) n00b_vks_backend_init(n00b_vks_backend_t *be);

/**
 * @brief Tear down a backend: call ops->cleanup.
 * @post be->ctx is nullptr.
 */
extern void n00b_vks_backend_cleanup(n00b_vks_backend_t *be);
