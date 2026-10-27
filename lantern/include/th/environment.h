#ifndef TH_ENVIRONMENT_H
#define TH_ENVIRONMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/object_archive.h"
#include "diag/diagnostics.h"
#include "lib/buffer.h"
#include "lib/string.h"
#include "th/typehash.h"

typedef enum {
  TH_ENV_PRIVATE,
  TH_ENV_PACKAGE,
  TH_ENV_PUBLIC,
} th_env_visibility_t;

typedef struct {
  core_string_t qualified_name;
  core_string_t root_name;
  core_string_t module;
  core_string_t package;
  th_type *signature;
  th_type *error_type;
  th_type *contract;
} th_env_method_t;

typedef struct {
  core_string_t trait_name;
  th_type *signature;
  th_type *error_type;
  th_type *contract;
} th_env_method_requirement_t;

typedef struct {
  th_type *receiver_type;
  th_env_method_t method;
} th_env_dispatch_entry_t;

typedef struct {
  core_string_t method_name;
  th_env_method_t implementation;
} th_env_vtable_slot_t;

typedef struct {
  core_string_t trait_name;
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
  core_string_t identity;
  core_string_t metadata_key;
} th_env_compose_error_t;

typedef struct {
  th_env_metadata_owner_t owner_kind;
  core_string_t owner_name;
  core_string_t key;
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
  TH_ENV_TYPE_SHAPE_OBJECT,
} th_env_type_shape_kind_t;

typedef struct {
  th_env_declaration_kind_t kind;
  core_string_t qualified_name;
  core_string_t root_name;
  core_string_t module;
  core_string_t package;
  th_env_visibility_t visibility;
  th_type *contract;
  th_env_type_shape_kind_t type_shape_kind;
  th_type *type_shape;
  th_type *value_type;
} th_env_declaration_view_t;

th_env *th_env_new(void);
th_env *th_env_new_in_context(th_ctx *types);
void th_env_free(th_env *environment);

th_env *th_env_compose(const th_env *const *environments, size_t count);
th_env *th_env_compose_diagnose(const th_env *const *environments,
                                size_t count, th_env_compose_error_t *error);
th_env *th_env_compose_report(const th_env *const *environments, size_t count,
                              core_diagnostics_t *diagnostics,
                              th_env_compose_error_t *error);
th_env *th_env_compose_report_in_context(
    const th_env *const *environments, size_t count, th_ctx *types,
    core_diagnostics_t *diagnostics, th_env_compose_error_t *error);

bool th_env_save(const th_env *environment, core_string_t path,
                 size_t mapping_capacity,
                 core_object_archive_error_t *error);
th_env *th_env_load(core_string_t path, core_object_archive_error_t *error);

typedef struct {
  bool (*module)(void *context, core_string_t module);
  bool (*metadata)(void *context, th_env_metadata_owner_t kind,
                   core_string_t owner);
  bool (*intrinsic)(void *context, core_string_t trait,
                    const th_type *receiver);
  void *context;
} th_env_filter_t;

th_env *th_env_subset(const th_env *source, const th_env_filter_t *filter);
bool th_env_encode(const th_env *environment, core_buffer_t *out);
th_env *th_env_decode(th_ctx *context, const uint8_t *data, size_t length);

size_t th_env_declaration_count(const th_env *environment);
bool th_env_declaration_at(const th_env *environment, size_t index,
                           th_env_declaration_view_t *out);

bool th_env_register_type(th_env *environment,
                          core_string_t qualified_type_name,
                          core_string_t module, core_string_t package,
                          th_env_visibility_t visibility);
bool th_env_register_type_shape(th_env *environment,
                                core_string_t qualified_type_name,
                                th_env_type_shape_kind_t shape_kind,
                                th_type *shape);
th_type *th_env_type_shape(const th_ctx *context,
                           core_string_t qualified_type_name,
                           th_env_type_shape_kind_t *shape_kind);
bool th_env_register_trait(th_env *environment, core_string_t qualified_name,
                           core_string_t module, core_string_t package,
                           th_env_visibility_t visibility, th_type *methods);
bool th_env_register_trait_channels(
    th_env *environment, core_string_t qualified_name, core_string_t module,
    core_string_t package, th_env_visibility_t visibility, th_type *methods,
    th_type *errors);
bool th_env_register_trait_contracts(
    th_env *environment, core_string_t qualified_name, core_string_t module,
    core_string_t package, th_env_visibility_t visibility,
    th_type *method_contracts);
bool th_env_register_intrinsic_type_pattern(th_env *environment,
                                           core_string_t trait_name,
                                           th_type *receiver_pattern);
bool th_env_register_function(th_env *environment,
                              core_string_t qualified_name,
                              core_string_t root_name, core_string_t module,
                              core_string_t package,
                              th_env_visibility_t visibility,
                              th_type *signature);
bool th_env_register_function_channels(
    th_env *environment, core_string_t qualified_name, core_string_t root_name,
    core_string_t module, core_string_t package,
    th_env_visibility_t visibility, th_type *signature, th_type *error_type);
bool th_env_register_function_contract(
    th_env *environment, core_string_t qualified_name, core_string_t root_name,
    core_string_t module, core_string_t package,
    th_env_visibility_t visibility, th_type *contract);
bool th_env_set_function_required(th_env *environment,
                                  core_string_t qualified_name,
                                  size_t required);
size_t th_env_function_required(const th_env *environment,
                                core_string_t qualified_name);
bool th_env_register_value(th_env *environment, core_string_t qualified_name,
                           core_string_t root_name, core_string_t module,
                           core_string_t package,
                           th_env_visibility_t visibility,
                           th_type *value_type);
th_type *th_env_signature_contract(th_ctx *context, th_type *signature,
                                   th_type *error_type);
bool th_env_signature_channels(th_ctx *context, th_type *contract,
                               th_type **signature, th_type **error_type);
bool th_env_register_metadata(th_env *environment,
                              th_env_metadata_owner_t owner_kind,
                              core_string_t owner_name, core_string_t key,
                              uint32_t version, const void *data, size_t size);
bool th_env_replace_metadata(th_env *environment,
                             th_env_metadata_owner_t owner_kind,
                             core_string_t owner_name, core_string_t key,
                             uint32_t version, const void *data, size_t size);
size_t th_env_metadata_count(const th_env *environment);
bool th_env_metadata_at(const th_env *environment, size_t index,
                        th_env_metadata_view_t *out);
bool th_env_freeze(th_env *environment);
bool th_env_is_frozen(const th_env *environment);

bool th_ctx_set_environment(th_ctx *context, const th_env *environment,
                            core_string_t module, core_string_t package);
const th_env *th_ctx_environment(const th_ctx *context);
bool th_ctx_set_environment_scope(th_ctx *context, core_string_t module,
                                 core_string_t package);
bool th_env_type_visible(const th_ctx *context,
                         core_string_t qualified_type_name);
bool th_env_implements(th_ctx *context, th_type *receiver,
                       core_string_t trait_name);
bool th_env_method_lookup(th_ctx *context, th_type *receiver,
                          core_string_t trait_name, core_string_t method_name,
                          th_env_method_t *method);
th_env_method_status_t th_env_method_resolve(th_ctx *context, th_type *receiver,
                                             core_string_t trait_name,
                                             core_string_t method_name,
                                             th_env_method_t *method);
th_env_method_status_t th_env_method_resolve_by_name(
    th_ctx *context, th_type *receiver, core_string_t method_name,
    th_env_method_t *method, core_string_t *trait_name);
bool th_env_type_pattern_subsumes(const th_type *general,
                                  const th_type *specific);
bool th_env_type_pattern_accepts(th_ctx *context, const th_type *pattern,
                                 const th_type *candidate);
size_t th_env_method_candidates(th_ctx *context, core_string_t trait_name,
                                core_string_t method_name,
                                th_env_method_t *methods, size_t max_methods);
th_env_method_status_t th_env_method_plan_dispatch(
    th_ctx *context, core_string_t trait_name, core_string_t method_name,
    th_type *const *receiver_types, size_t receiver_count,
    th_env_dispatch_entry_t *entries, size_t capacity,
    size_t *required_count, bool *direct_call);
th_env_method_status_t th_env_trait_vtable_plan(
    th_ctx *context, core_string_t trait_name, th_type *concrete_type,
    th_env_vtable_slot_t *slots, size_t capacity, size_t *required_count,
    th_env_vtable_plan_t *plan);
size_t th_env_method_requirements(
    th_ctx *context, core_string_t method_name,
    th_env_method_requirement_t *requirements, size_t max_requirements);

#endif
