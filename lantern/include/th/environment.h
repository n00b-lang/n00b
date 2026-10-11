#ifndef TH_ENVIRONMENT_H
#define TH_ENVIRONMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/object_archive.h"
#include "lib/diagnostic.h"
#include "th/typehash.h"

typedef enum {
  TH_ENV_PRIVATE,
  TH_ENV_PACKAGE,
  TH_ENV_PUBLIC,
} th_env_visibility_t;

typedef struct {
  const char *qualified_name;
  const char *root_name;
  const char *module;
  const char *package;
  th_type *signature;
  th_type *error_type;
  /* Canonical callable contract; jointly quantifies both channels. */
  th_type *contract;
} th_env_method_t;

typedef struct {
  const char *trait_name;
  th_type *signature;
  th_type *error_type;
  th_type *contract;
} th_env_method_requirement_t;

typedef struct {
  th_type *receiver_type;
  th_env_method_t method;
} th_env_dispatch_entry_t;

typedef struct {
  const char *method_name;
  th_env_method_t implementation;
} th_env_vtable_slot_t;

typedef struct {
  const char *trait_name;
  th_type *concrete_type;
  uint64_t concrete_type_hash;
  th_env_vtable_slot_t *slots;
  size_t slot_count;
  size_t required_slot_count;
} th_env_vtable_plan_t;

typedef enum {
  TH_ENV_METHOD_NOT_FOUND,
  TH_ENV_METHOD_FOUND,
  TH_ENV_METHOD_AMBIGUOUS,
} th_env_method_status_t;

typedef enum {
  TH_ENV_METADATA_TYPE,
  TH_ENV_METADATA_TRAIT,
  TH_ENV_METADATA_FUNCTION,
  TH_ENV_METADATA_VALUE,
  TH_ENV_METADATA_ENVIRONMENT,
} th_env_metadata_owner_t;

typedef enum {
  TH_ENV_COMPOSE_OK,
  TH_ENV_COMPOSE_INVALID_SOURCE,
  TH_ENV_COMPOSE_TYPE_CONFLICT,
  TH_ENV_COMPOSE_TRAIT_CONFLICT,
  TH_ENV_COMPOSE_FUNCTION_CONFLICT,
  TH_ENV_COMPOSE_METADATA_CONFLICT,
  TH_ENV_COMPOSE_VALUE_CONFLICT,
} th_env_compose_status_t;

typedef struct {
  th_env_compose_status_t status;
  const char *identity;
  const char *metadata_key;
} th_env_compose_error_t;

typedef struct {
  th_env_metadata_owner_t owner_kind;
  const char *owner_name;
  const char *key;
  uint32_t version;
  const uint8_t *data;
  size_t size;
} th_env_metadata_view_t;

typedef enum {
  TH_ENV_DECLARATION_TYPE,
  TH_ENV_DECLARATION_TRAIT,
  TH_ENV_DECLARATION_FUNCTION,
  TH_ENV_DECLARATION_VALUE,
} th_env_declaration_kind_t;

typedef enum {
  TH_ENV_TYPE_SHAPE_NONE,
  TH_ENV_TYPE_SHAPE_RECORD,
  TH_ENV_TYPE_SHAPE_VARIANT,
  TH_ENV_TYPE_SHAPE_ENUM,
  TH_ENV_TYPE_SHAPE_ALIAS,
  TH_ENV_TYPE_SHAPE_NOMINAL,
  TH_ENV_TYPE_SHAPE_BITSET,
} th_env_type_shape_kind_t;

/* Stable declaration metadata exposed to language frontends when constructing
 * imported symbol tables. `contract` is null for types, a jointly quantified
 * method-contract record for traits, and a jointly quantified callable
 * contract for functions. All strings/types are borrowed from the frozen
 * environment and remain valid while it is alive. */
typedef struct {
  th_env_declaration_kind_t kind;
  const char *qualified_name;
  const char *root_name;
  const char *module;
  const char *package;
  th_env_visibility_t visibility;
  th_type *contract;
  th_env_type_shape_kind_t type_shape_kind;
  th_type *type_shape;
  th_type *value_type;
} th_env_declaration_view_t;


/* This is a language-neutral environment: callers provide stable qualified
 * identities and resolved visibility. Source spelling, import lookup, and
 * name mangling remain frontend responsibilities. Signature terms are
 * borrowed immutable schemes. Their owning module/type contexts must outlive
 * the environment; the environment must outlive every inference context
 * attached to it. Publish registrations only after their schemes are
 * finalized, then freeze before concurrent queries. A frozen environment
 * supports concurrent queries from distinct th_ctx instances; callers must
 * not mutate or destroy it while queries are in flight. */
th_env *th_env_new(void);
/* Create an environment whose type allocations use a caller-owned universe. */
th_env *th_env_new_in_context(th_ctx *types);
void th_env_free(th_env *environment);

/* Compose frozen library environments into a new mutable environment. The
 * source snapshots remain unchanged; their borrowed signature contexts must
 * outlive the composed environment. A loaded environment must likewise remain
 * alive while a composed snapshot borrows its schemes. Add newly linked
 * declarations (including open-trait contributions) to the result, then
 * freeze it before publishing for queries. Do not mutate a published snapshot;
 * link further contributions by composing a fresh environment.
 * Duplicate or conflicting declarations make composition fail and return NULL. */
th_env *th_env_compose(const th_env *const *environments, size_t count);
/* On failure, error identifies the conflicting stable declaration/metadata
 * owner and borrows those strings from the source environment. */
th_env *th_env_compose_diagnose(const th_env *const *environments,
                                size_t count, th_env_compose_error_t *error);
/* Also reports composition conflicts into Lantern's shared diagnostic
 * collection. Structured conflict details remain available through `error`. */
th_env *th_env_compose_report(const th_env *const *environments, size_t count,
                              core_diagnostics_t *diagnostics,
                              th_env_compose_error_t *error);
th_env *th_env_compose_report_in_context(
    const th_env *const *environments, size_t count, th_ctx *types,
    core_diagnostics_t *diagnostics, th_env_compose_error_t *error);

/* Save requires a frozen environment. A zero mapping_capacity selects an
 * automatic size with transient room for reconstituting the loaded graph. */
bool th_env_save(const th_env *environment, const char *path,
                 size_t mapping_capacity,
                 core_object_archive_error_t *error);
th_env *th_env_load(const char *path, core_object_archive_error_t *error);

size_t th_env_declaration_count(const th_env *environment);
bool th_env_declaration_at(const th_env *environment, size_t index,
                           th_env_declaration_view_t *out);

bool th_env_register_type(th_env *environment, const char *qualified_type_name,
                          const char *module, const char *package,
                          th_env_visibility_t visibility);
/* A structural type definition is borrowed like a signature term and must
 * remain alive while the environment is queried. It is archived with the
 * frozen environment and reconciled when snapshots are composed. */
bool th_env_register_type_shape(th_env *environment,
                                const char *qualified_type_name,
                                th_env_type_shape_kind_t shape_kind,
                                th_type *shape);
th_type *th_env_type_shape(const th_ctx *context,
                           const char *qualified_type_name,
                           th_env_type_shape_kind_t *shape_kind);
/* Trait method signatures use Lantern's canonical receiver-first shape:
 * parameter zero is the receiver, and its type must be an unbound type
 * variable. A frontend for a language with another receiver convention can
 * normalize the signature and provide an adapter for its source ABI. */
bool th_env_register_trait(th_env *environment, const char *qualified_name,
                           const char *module, const char *package,
                           th_env_visibility_t visibility, th_type *methods);
/* `methods` and `errors` are records with the same method names. The error
 * entries are attached to each function type before the reusable method
 * patterns are registered together. */
bool th_env_register_trait_channels(
    th_env *environment, const char *qualified_name, const char *module,
    const char *package, th_env_visibility_t visibility, th_type *methods,
    th_type *errors);
/* The method map holds reusable function-type patterns, including their
 * failure channels, with each value built by th_env_signature_contract().
 * Matching clones open graphs; explicit binders remain supported for
 * frontends that choose to provide them. */
bool th_env_register_trait_contracts(
    th_env *environment, const char *qualified_name, const char *module,
    const char *package, th_env_visibility_t visibility,
    th_type *method_contracts);
/* Register an intrinsic type-pattern fact for a methodless marker trait.
 * This is for language/runtime builtins whose implementation cannot be
 * expressed as an ordinary function signature. Patterns are borrowed and
 * universally instantiated for each query; ordinary traits must continue to
 * derive implementations from matching functions. */
bool th_env_register_intrinsic_type_pattern(th_env *environment,
                                           const char *trait_name,
                                           th_type *receiver_pattern);
bool th_env_register_function(th_env *environment, const char *qualified_name,
                              const char *root_name, const char *module,
                              const char *package,
                              th_env_visibility_t visibility,
                              th_type *signature);
bool th_env_register_function_channels(
    th_env *environment, const char *qualified_name, const char *root_name,
    const char *module, const char *package,
    th_env_visibility_t visibility, th_type *signature, th_type *error_type);
bool th_env_register_function_contract(
    th_env *environment, const char *qualified_name, const char *root_name,
    const char *module, const char *package,
    th_env_visibility_t visibility, th_type *contract);
bool th_env_register_value(th_env *environment, const char *qualified_name,
                           const char *root_name, const char *module,
                           const char *package, th_env_visibility_t visibility,
                           th_type *value_type);
th_type *th_env_signature_contract(th_ctx *context, th_type *signature,
                                   th_type *error_type);
/* A callable contract is a function type with an intrinsic failure channel.
 * A nil failure type means that the callable has no failure exits. */
bool th_env_signature_channels(th_ctx *context, th_type *contract,
                               th_type **signature, th_type **error_type);
/* Metadata keys are producer-namespaced; each key has its own schema version.
 * Environment metadata names a logical manifest or contribution rather than
 * a declaration. Payloads are opaque copied bytes and must not contain
 * process-local pointers. Views borrow storage owned by the environment. */
bool th_env_register_metadata(th_env *environment,
                              th_env_metadata_owner_t owner_kind,
                              const char *owner_name, const char *key,
                              uint32_t version, const void *data, size_t size);
size_t th_env_metadata_count(const th_env *environment);
bool th_env_metadata_at(const th_env *environment, size_t index,
                        th_env_metadata_view_t *out);
/* Finalize a complete link/import closure. Ordinary functions whose complete
 * success/failure contracts match trait requirements contribute candidates.
 * The logic relation indexes only stable (trait, method, function) identities;
 * it does not encode type variables, substitutions, constraints, or
 * specificity. Candidate matching and receiver-dependent selection remain in
 * the type layer. Only direct signature matches are derived: this layer does
 * not infer trait inheritance or other implication edges. The frozen
 * environment is immutable and safe for concurrent queries; publish a newly
 * composed snapshot to add more open-trait facts. */
bool th_env_freeze(th_env *environment);
bool th_env_is_frozen(const th_env *environment);

/* Attach once, after the environment is frozen. It remains borrowed. */
bool th_ctx_set_environment(th_ctx *context, const th_env *environment,
                            const char *module, const char *package);
const th_env *th_ctx_environment(const th_ctx *context);
/* Changes the visibility perspective while retaining the attached snapshot. */
bool th_ctx_set_environment_scope(th_ctx *context, const char *module,
                                 const char *package);
bool th_env_type_visible(const th_ctx *context,
                         const char *qualified_type_name);
bool th_env_implements(th_ctx *context, th_type *receiver,
                       const char *trait_name);
/* Resolves only against the receiver's concrete type. Origin and import
 * ordering only break ties between equivalent patterns; incomparable maximal
 * patterns are reported as ambiguous. */
bool th_env_method_lookup(th_ctx *context, th_type *receiver,
                          const char *trait_name, const char *method_name,
                          th_env_method_t *method);
th_env_method_status_t th_env_method_resolve(th_ctx *context, th_type *receiver,
                                             const char *trait_name,
                                             const char *method_name,
                                             th_env_method_t *method);
/* Resolve receiver.method_name across all visible trait contracts declaring
 * that method. A function satisfying several contracts is one target; distinct
 * applicable functions or an ambiguous per-trait match return AMBIGUOUS. */
th_env_method_status_t th_env_method_resolve_by_name(
    th_ctx *context, th_type *receiver, const char *method_name,
    th_env_method_t *method, const char **trait_name);
/* Structural pattern subsumption; constraints on variables are not checked.
 * Neither input is bound. */
bool th_env_type_pattern_subsumes(const th_type *general,
                                  const th_type *specific);
/* Test a pattern against a fully concrete candidate, including the pattern's
 * variable constraints. Neither input is bound. */
bool th_env_type_pattern_accepts(th_ctx *context, const th_type *pattern,
                                 const th_type *candidate);
/* Enumerate every visible signature-compatible implementation of a trait
 * method. The logic relation indexes declarations; generic instantiation,
 * concrete receiver matching, constraints, and specificity remain in the type
 * layer. This is the candidate set for building a trait-value runtime dispatch
 * table; unlike method_resolve(), it does not select for one receiver type.
 * Returns the total count even when `max_methods` is smaller. */
size_t th_env_method_candidates(th_ctx *context, const char *trait_name,
                                const char *method_name,
                                th_env_method_t *methods, size_t max_methods);
/* Resolve one target for every concrete receiver type a compiler knows can
 * reach a trait-valued call. On success, entries preserve receiver order;
 * `required_count` reports the full size even when capacity is smaller. If
 * every receiver selects the same function, `direct_call` is true and codegen
 * may omit a runtime type switch. No partial plan is written on failure. */
th_env_method_status_t th_env_method_plan_dispatch(
    th_ctx *context, const char *trait_name, const char *method_name,
    th_type *const *receiver_types, size_t receiver_count,
    th_env_dispatch_entry_t *entries, size_t capacity,
    size_t *required_count, bool *direct_call);
/* Plan every slot of one concrete type's trait vtable, in trait declaration
 * order. Each slot uses ordinary most-specific method resolution. The full
 * plan is validated before any caller buffer is written; required_count is
 * set only on success and may exceed capacity. */
th_env_method_status_t th_env_trait_vtable_plan(
    th_ctx *context, const char *trait_name, th_type *concrete_type,
    th_env_vtable_slot_t *slots, size_t capacity, size_t *required_count,
    th_env_vtable_plan_t *plan);
/* Enumerate visible trait contracts declaring `method_name`. Returned
 * signatures are fresh instances allocated in `context`; each call is
 * independent, while type-variable relationships within one trait contract
 * are preserved. Names borrow from the frozen environment. Returns the total
 * count even when `max_requirements` is smaller. */
size_t th_env_method_requirements(
    th_ctx *context, const char *method_name,
    th_env_method_requirement_t *requirements, size_t max_requirements);

#endif
