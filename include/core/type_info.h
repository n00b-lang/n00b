/**
 * @file type_info.h
 * @brief Type registry: per-type metadata and runtime type lookup.
 *
 * Every registered type gets an `n00b_type_info_t` that carries its name,
 * a fixed-size core vtable of bare function pointers (known signatures),
 * and an optional extension vtable with full signature metadata per method.
 *
 * The registry is a dict mapping `typehash(T)` -> `n00b_type_info_t *`,
 * stored on the runtime and initialized during `n00b_init()`.
 */
#pragma once

#include "n00b.h"
#include "core/vtable.h"
#include "adt/option.h"

/**
 * @brief Classification of literal syntax for a type.
 */
typedef enum n00b_literal_kind_t : uint8_t {
    N00B_LIT_NONE = 0,
    N00B_LIT_PRIMITIVE,
    N00B_LIT_PREFIX,
    N00B_LIT_CONTAINER,
} n00b_literal_kind_t;

typedef n00b_option_t(n00b_array_t(n00b_method_t) *) n00b_ext_vtable_opt_t;

/**
 * @brief Per-type metadata: name, vtables, allocation size, literal info.
 *
 * `core_vtable` is a fixed-size array of bare function pointers indexed by
 * `n00b_builtin_type_fn` -- always present, signatures are known at compile
 * time.
 *
 * `ext_vtable` is an optional pointer to an `n00b_array_t(n00b_method_t)` --
 * none by default, set only for types with custom extension methods, where
 * each method carries full signature metadata.
 *
 * `lock_offset` is the byte offset of the `n00b_rwlock_t *` field in the
 * type's struct, or none if the type has no lock.  Used by the central
 * finalizer to clean up locks without per-object finalizer registration.
 */
typedef struct n00b_type_info_t {
    const char              *name;
    n00b_vtable_entry        core_vtable[N00B_BI_NUM_FUNCS];
    n00b_ext_vtable_opt_t    ext_vtable;
    uint32_t                 alloc_len;
    n00b_option_t(uint32_t)  lock_offset;
    n00b_literal_kind_t      literal_kind;
    const char              *literal_modifier;
    bool                     ctor_takes_kargs;  /**< ctor(self, kargs) */
    bool                     ctor_takes_vargs;  /**< ctor(self, vargs, kargs) */
} n00b_type_info_t;

// ============================================================================
// Registry API
// ============================================================================

/**
 * @brief Initialize the type registry (called from n00b_init).
 *
 * Allocates the registry dict from the system pool and registers
 * all built-in types.
 */
extern void n00b_type_registry_init(void);

/**
 * @brief Register a type in the global registry.
 * @param type_hash typehash(T) for the type.
 * @param info      Type info to register (deep-copied into the registry).
 * @return true on success, false if the type_hash is already registered.
 */
extern bool n00b_type_register(uint64_t type_hash, const n00b_type_info_t *info);

/**
 * @brief Look up type info by typehash.
 * @param type_hash typehash(T) to look up.
 * @return The registered type info, or none if not found.
 */
extern n00b_option_t(n00b_type_info_t *) n00b_type_lookup(uint64_t type_hash);

/**
 * @brief Get the typehash stored in an object's allocation header.
 * @param obj Pointer to a managed allocation.
 * @return The typehash, or 0 if the object has no allocation info.
 */
extern uint64_t n00b_obj_typehash(void *obj);

/**
 * @brief Get the type info for a managed object.
 * @param obj Pointer to a managed allocation.
 * @return Type info, or none if unregistered or not a managed object.
 */
extern n00b_option_t(n00b_type_info_t *) n00b_type_info_for(void *obj);

/**
 * @brief Add an extension method to a registered type.
 * @param type_hash typehash(T) of the type to extend.
 * @param method    Method descriptor (deep-copied into the registry).
 * @return true on success, false if the type is not registered.
 */
extern bool n00b_type_add_method(uint64_t type_hash, const n00b_method_t *method);

/**
 * @brief Look up a core vtable entry for a managed object.
 * @param obj  Pointer to a managed allocation.
 * @param slot Core vtable slot index.
 * @return The function pointer, or nullptr if the slot is empty or the
 *         object's type is unregistered.
 */
static inline n00b_vtable_entry
n00b_obj_core_method(void *obj, enum n00b_builtin_type_fn slot)
{
    auto info_opt = n00b_type_info_for(obj);

    if (!n00b_option_is_set(info_opt) || slot >= N00B_BI_NUM_FUNCS) {
        return nullptr;
    }

    return n00b_option_get(info_opt)->core_vtable[slot];
}

// ============================================================================
// Registration macros
// ============================================================================

/**
 * @brief Register a type with a pre-built type info struct.
 *
 * Usage:
 * ```c
 * N00B_TYPE_REGISTER(my_type_t,
 *     N00B_CORE_METHOD(N00B_BI_CONSTRUCTOR, my_ctor),
 *     N00B_CORE_METHOD(N00B_BI_FINALIZER, my_dtor),
 * );
 * ```
 */
#define N00B_TYPE_REGISTER(T, ...)                                                             \
    n00b_type_register(typehash(T *),                                                          \
                       &(n00b_type_info_t){                                                    \
                           .name      = N00B_TO_STRING(T),                                     \
                           .alloc_len = sizeof(T),                                             \
                           __VA_ARGS__                                                         \
                       })

/**
 * @brief Set a core vtable slot (use inside N00B_TYPE_REGISTER).
 */
#define N00B_CORE_METHOD(slot, fn_ptr) .core_vtable[slot] = (n00b_vtable_entry)(fn_ptr)

/**
 * @brief Set the lock field offset (use inside N00B_TYPE_REGISTER).
 *
 * Tells the central finalizer where to find the `n00b_rwlock_t *`
 * field so it can free the lock without per-object registration.
 */
#define N00B_LOCK_FIELD(T, field) .lock_offset = n00b_option_set(uint32_t, (uint32_t)offsetof(T, field))

/** @brief Mark the constructor as taking kargs: `ctor(self, kargs)`. */
#define N00B_CTOR_KARGS .ctor_takes_kargs = true

/** @brief Mark the constructor as taking vargs+kargs: `ctor(self, vargs, kargs)`. */
#define N00B_CTOR_VARGS .ctor_takes_vargs = true, .ctor_takes_kargs = true

/**
 * @brief Register all built-in types (called by n00b_type_registry_init).
 */
extern void n00b_register_builtin_types(void);
