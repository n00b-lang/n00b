/**
 * @file vks.h
 * @brief Virtual KV store — a parameterized container peer to n00b_dict_t(K,V).
 *
 * @c n00b_vks_store_t(K,V) is a typed key/value store: it EMBEDS a native
 * @c n00b_dict_t(K,V) (the in-memory index) and DELEGATES all typed
 * operations to it, while layering a mutation hook and (Phase 2) a pluggable
 * durability @c backend on top.  It does NOT reimplement a hash table.
 *
 * Like @c n00b_dict_t / @c n00b_list_t, the type is realized with ncc's
 * @c _generic_struct + @c typeid() machinery and needs NO builtin-type
 * registry entry.  Type safety for the typed op macros comes for free from
 * the embedded dict's own @c typeid() checks.
 *
 * **Phase 1 scope:** in-memory parameterized type only.  The @c backend field
 * is declared so the struct layout is stable, but it stays nullptr and is
 * never invoked.  @c n00b_vks_flush is a no-op (returns ok) when there is no
 * backend; it only resets the dirty counter.
 *
 * Usage:
 * @code
 *     n00b_vks_store_t(n00b_string_t *, n00b_string_t *) *s =
 *         n00b_vks_store_new(n00b_string_t *, n00b_string_t *);
 *     n00b_vks_put(s, key, val);
 *     bool found;
 *     n00b_string_t *v = n00b_vks_get(s, key, &found);
 * @endcode
 */
#pragma once

#include "n00b.h"
#include "core/macros.h"
#include "core/alloc.h"
#include "adt/dict.h"
#include "adt/list.h"
#include "adt/result.h"
#include "vks/types.h"
#include "vks/backend.h"

// ============================================================================
// Defaults
// ============================================================================

#if !defined(N00B_VKS_DEFAULT_DIRTY_MAX)
#define N00B_VKS_DEFAULT_DIRTY_MAX 64
#endif

// ============================================================================
// Type definition
// ============================================================================

#define n00b_vks_store_tid(k, v) typeid("n00b_vks_store", k, v)

/**
 * @brief Reference a VKS store type for key type @p k and value type @p v.
 * @param k  Key type.
 * @param v  Value type.
 *
 * Struct layout (kept byte-compatible with @c _n00b_vks_store_internal_t):
 *   @c { n00b_dict_t(k,v) *mem; n00b_vks_backend_t *backend;
 *        uint32_t dirty; uint32_t dirty_max;
 *        n00b_allocator_t *allocator; }
 */
#define n00b_vks_store_t(k, v)                                                                  \
    _generic_struct n00b_vks_store_tid(k, v)                                                    \
    {                                                                                          \
        n00b_dict_t(k, v) *mem;                                                                \
        n00b_vks_backend_t *backend;                                                           \
        uint32_t            dirty;                                                             \
        uint32_t            dirty_max;                                                         \
        n00b_allocator_t   *allocator;                                                         \
    }

/**
 * @brief Type-erased view of a VKS store for the generic functions.
 *
 * The field prefix MUST match @c n00b_vks_store_t(k,v) field-for-field so that
 * a cast from the typed struct pointer is sound (mirrors dict's
 * @c _n00b_dict_internal_t pattern).
 */
typedef struct {
    void               *mem;
    n00b_vks_backend_t *backend;
    uint32_t            dirty;
    uint32_t            dirty_max;
    n00b_allocator_t   *allocator;
} _n00b_vks_store_internal_t;

// ============================================================================
// Internal helpers (not part of the public API)
// ============================================================================

/**
 * @internal Structural check: the typed store and the erased view must agree on
 *           size and on the offsets of every field touched by a cast.
 */
#define _n00b_vks_structural_check(store_ptr)                                                   \
    static_assert(sizeof(*(store_ptr)) == sizeof(_n00b_vks_store_internal_t));                  \
    static_assert(offsetof(typeof(*(store_ptr)), mem)                                           \
                  == offsetof(_n00b_vks_store_internal_t, mem));                                \
    static_assert(offsetof(typeof(*(store_ptr)), backend)                                       \
                  == offsetof(_n00b_vks_store_internal_t, backend));                            \
    static_assert(offsetof(typeof(*(store_ptr)), dirty)                                         \
                  == offsetof(_n00b_vks_store_internal_t, dirty));                              \
    static_assert(offsetof(typeof(*(store_ptr)), dirty_max)                                     \
                  == offsetof(_n00b_vks_store_internal_t, dirty_max));                          \
    static_assert(offsetof(typeof(*(store_ptr)), allocator)                                     \
                  == offsetof(_n00b_vks_store_internal_t, allocator))

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Kwargs carrier for @c n00b_vks_store_new.
 *
 * (A plain designated-initializer struct so the @c _new macro can accept
 *  @c .backend / @c .allocator / @c .dirty_max in any order.)
 */
typedef struct {
    n00b_vks_backend_t *backend;
    n00b_allocator_t   *allocator;
    uint32_t            dirty_max;
} _n00b_vks_new_opts_t;

/**
 * @brief Allocate and initialize a new typed VKS store.
 *
 * Allocates the generic store struct, builds an embedded
 * @c n00b_dict_new(K, V) as the in-memory index, and wires up optional
 * backend / allocator / dirty_max kwargs.
 *
 * @param K    Key type.
 * @param V    Value type.
 * @param ...  Optional kwargs:
 *             @c .backend   (n00b_vks_backend_t*, default nullptr — Phase 1),
 *             @c .allocator (n00b_allocator_t*,  default nullptr),
 *             @c .dirty_max (uint32_t, default N00B_VKS_DEFAULT_DIRTY_MAX).
 */
#define n00b_vks_store_new(K, V, ...)                                                           \
    ({                                                                                         \
        _n00b_vks_new_opts_t _bl_o = (_n00b_vks_new_opts_t){                                    \
            .dirty_max = N00B_VKS_DEFAULT_DIRTY_MAX,                                            \
            __VA_OPT__(__VA_ARGS__)};                                                           \
        n00b_vks_store_t(K, V) *_bl_s = n00b_alloc(n00b_vks_store_t(K, V),                      \
                                                   .allocator = _bl_o.allocator);              \
        _bl_s->mem       = n00b_dict_new(K, V, .allocator = _bl_o.allocator);                   \
        _bl_s->backend   = _bl_o.backend;                                                       \
        _bl_s->dirty     = 0;                                                                   \
        _bl_s->dirty_max = _bl_o.dirty_max;                                                     \
        _bl_s->allocator = _bl_o.allocator;                                                     \
        _n00b_vks_structural_check(_bl_s);                                                      \
        _n00b_vks_post_open((_n00b_vks_store_internal_t *)_bl_s);                                \
        _bl_s;                                                                                  \
    })

// ============================================================================
// Typed operations — DELEGATE to the embedded dict + fire the mutation hook
// ============================================================================

/**
 * @brief Store @p val under @p key, overwriting any existing entry.
 *
 * The embedded @c n00b_dict_put returns a pointer to the displaced value (or
 * nullptr on first insert), NOT a success flag — a put always stores.  We
 * therefore expose this as @c void: the meaningful effect is the mutation
 * (which fires the dirty hook).
 */
#define n00b_vks_put(s, key, val)                                                               \
    do {                                                                                       \
        _n00b_vks_structural_check(s);                                                          \
        (void)n00b_dict_put((s)->mem, key, val);                                                \
        _n00b_vks_after_mutation((_n00b_vks_store_internal_t *)(s));                            \
    } while (0)

/**
 * @brief Fetch the value stored under @p key.
 * @param found  Out-param: set true iff the key was present.
 * @return The value (or a zero value when absent), per @c n00b_dict_get.
 */
#define n00b_vks_get(s, key, found) n00b_dict_get((s)->mem, key, found)

/**
 * @brief Remove @p key.
 * @return The bool returned by the embedded @c n00b_dict_remove.
 */
#define n00b_vks_del(s, key)                                                                    \
    ({                                                                                         \
        _n00b_vks_structural_check(s);                                                          \
        bool _bl_r = n00b_dict_remove((s)->mem, key);                                           \
        _n00b_vks_after_mutation((_n00b_vks_store_internal_t *)(s));                            \
        _bl_r;                                                                                  \
    })

/**
 * @brief Store @p val under @p key only when @p key is absent.
 *
 * Implemented via the embedded dict's @c n00b_dict_add (add-if-absent).
 * @return true if stored; false if @p key already existed.
 */
#define n00b_vks_put_if_absent(s, key, val)                                                     \
    ({                                                                                         \
        _n00b_vks_structural_check(s);                                                          \
        bool _bl_r = n00b_dict_add((s)->mem, key, val);                                         \
        if (_bl_r) {                                                                            \
            _n00b_vks_after_mutation((_n00b_vks_store_internal_t *)(s));                        \
        }                                                                                       \
        _bl_r;                                                                                  \
    })

/**
 * @brief Test whether @p key is present.
 */
#define n00b_vks_contains(s, key) n00b_dict_contains((s)->mem, key)

// ============================================================================
// Generic (type-erased) operations
// ============================================================================

/**
 * @brief Rehydrate the store from its backend immediately after construction.
 *
 * Invoked at the end of @c n00b_vks_store_new (after the empty in-memory dict is
 * built).  When a backend with a @c load op is attached, it is asked to load
 * the durable image into the store; with no backend this is a no-op.  A missing
 * or unusable durable image is not an error — the store simply starts empty.
 *
 * @param s  Type-erased store (never nullptr).
 */
extern void _n00b_vks_post_open(_n00b_vks_store_internal_t *s);

/**
 * @brief Mutation hook: bump the dirty counter and, when a backend is attached
 *        and @c dirty has reached @c dirty_max, fire a full snapshot and reset
 *        @c dirty to 0.  With @c backend==nullptr it only counts.
 * @param s  Type-erased store (never nullptr).
 */
extern void _n00b_vks_after_mutation(_n00b_vks_store_internal_t *s);

/**
 * @brief Return all keys currently in the store as a list.
 *
 * Pointer-keyed stores only (the Phase 1 supported shape): the returned list
 * holds the key pointers.  Cast the result to your typed
 * @c n00b_list_t(K) * to consume it.
 *
 * @param store  Type-erased store pointer (a @c n00b_vks_store_t(k,v)*).
 */
extern n00b_result_t(n00b_list_t *) n00b_vks_keys(void *store);

/**
 * @brief Flush buffered mutations to the durability backend.
 *
 * Phase 1: when @c backend==nullptr this is a no-op that returns ok(true) and
 * resets the dirty counter to 0.
 *
 * @param store  Type-erased store pointer.
 */
extern n00b_result_t(bool) n00b_vks_flush(void *store);

/**
 * @brief Flush and release the store's backend (if any).
 * @param store  Type-erased store pointer.
 */
extern void n00b_vks_close(void *store);
