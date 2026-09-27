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
} th_env_method_t;

typedef struct {
  const char *trait_name;
  th_type *signature;
} th_env_method_requirement_t;

typedef enum {
  TH_ENV_METHOD_NOT_FOUND,
  TH_ENV_METHOD_FOUND,
  TH_ENV_METHOD_AMBIGUOUS,
} th_env_method_status_t;

typedef enum {
  TH_ENV_METADATA_TYPE,
  TH_ENV_METADATA_TRAIT,
  TH_ENV_METADATA_FUNCTION,
} th_env_metadata_owner_t;

typedef enum {
  TH_ENV_COMPOSE_OK,
  TH_ENV_COMPOSE_INVALID_SOURCE,
  TH_ENV_COMPOSE_TYPE_CONFLICT,
  TH_ENV_COMPOSE_TRAIT_CONFLICT,
  TH_ENV_COMPOSE_FUNCTION_CONFLICT,
  TH_ENV_COMPOSE_METADATA_CONFLICT,
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


/* Signature terms are borrowed immutable schemes. Their owning module/type
 * contexts must outlive the environment; the environment must outlive every
 * inference context attached to it. Publish registrations only after their
 * schemes are finalized, then freeze before concurrent queries. A frozen
 * environment supports concurrent queries from distinct th_ctx instances;
 * callers must not mutate or destroy it while queries are in flight. */
th_env *th_env_new(void);
void th_env_free(th_env *environment);

/* Compose frozen library environments into a new mutable environment. The
 * source snapshots remain unchanged; their borrowed signature contexts must
 * outlive the composed environment. A loaded environment must likewise remain
 * alive while a composed snapshot borrows its schemes. Add newly linked
 * declarations to the result, then freeze it before publishing for queries.
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

/* Save requires a frozen environment. A zero mapping_capacity selects an
 * automatic size with transient room for reconstituting the loaded graph. */
bool th_env_save(const th_env *environment, const char *path,
                 size_t mapping_capacity,
                 core_object_archive_error_t *error);
th_env *th_env_load(const char *path, core_object_archive_error_t *error);

bool th_env_register_type(th_env *environment, const char *qualified_type_name,
                          const char *module, const char *package,
                          th_env_visibility_t visibility);
bool th_env_register_trait(th_env *environment, const char *qualified_name,
                           const char *module, const char *package,
                           th_env_visibility_t visibility, th_type *methods);
bool th_env_register_function(th_env *environment, const char *qualified_name,
                              const char *root_name, const char *module,
                              const char *package,
                              th_env_visibility_t visibility,
                              th_type *signature);
/* Metadata keys are producer-namespaced; each key has its own schema version.
 * Payloads are opaque copied bytes and must not contain process-local pointers.
 * Views borrow storage owned by the environment. */
bool th_env_register_metadata(th_env *environment,
                              th_env_metadata_owner_t owner_kind,
                              const char *owner_name, const char *key,
                              uint32_t version, const void *data, size_t size);
size_t th_env_metadata_count(const th_env *environment);
bool th_env_metadata_at(const th_env *environment, size_t index,
                        th_env_metadata_view_t *out);
bool th_env_freeze(th_env *environment);
bool th_env_is_frozen(const th_env *environment);

/* Attach once, after the environment is frozen. It remains borrowed. */
bool th_ctx_set_environment(th_ctx *context, const th_env *environment,
                            const char *module, const char *package);
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
/* Enumerate every visible signature-compatible implementation of a trait
 * method. This is the candidate set for building a trait-value runtime
 * dispatch table; unlike method_resolve(), it does not select for one runtime
 * receiver type. Returns the total count even when `max_methods` is smaller. */
size_t th_env_method_candidates(th_ctx *context, const char *trait_name,
                                const char *method_name,
                                th_env_method_t *methods, size_t max_methods);
/* Enumerate visible trait contracts declaring `method_name`. Returned
 * signatures are fresh instances allocated in `context`; each call is
 * independent, while type-variable relationships within one trait contract
 * are preserved. Names borrow from the frozen environment. Returns the total
 * count even when `max_requirements` is smaller. */
size_t th_env_method_requirements(
    th_ctx *context, const char *method_name,
    th_env_method_requirement_t *requirements, size_t max_requirements);

#endif
