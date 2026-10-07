#include <string.h>

#include "lib/alloc.h"
#include "private/th/internal.h"

core_list_decl(th_member);

static th_env_trait_t *trait_named(const th_env *env, const char *name) {
  for (size_t i = 0; i < env->traits.len; i++)
    if (!strcmp(env->traits.data[i].name, name))
      return &env->traits.data[i];
  return nullptr;
}

static th_type *first_parameter(th_type *signature) {
  signature = th_resolve(signature);
  if (!signature || th_kind_of(signature) != TH_FUNCTION ||
      !th_func_nparams(signature))
    return nullptr;
  const th_param *param = th_func_param(signature, 0);
  return param ? param->type : nullptr;
}

static bool function_can_match(th_ctx *ctx, const th_env_trait_t *trait,
                               const th_env_function_t *function,
                               const char *method_name);

typedef const th_env_function_t *th_env_function_ptr_t;
core_list_decl(th_env_function_ptr_t);

typedef struct {
  const th_type *left;
  const th_type *right;
} type_pair_t;
core_list_decl(type_pair_t);

typedef struct {
  const th_type *variable;
  const th_type *value;
} type_binding_t;
core_list_decl(type_binding_t);

static bool pair_seen(core_list_t(type_pair_t) * seen, const th_type *a,
                      const th_type *b) {
  for (size_t i = 0; i < seen->len; i++)
    if (seen->data[i].left == a && seen->data[i].right == b)
      return true;
  core_list_push(*seen, ((type_pair_t){a, b}));
  return false;
}

static bool pattern_equal(const th_type *a, const th_type *b,
                          core_list_t(type_pair_t) * seen) {
  a = th_resolve_c(a);
  b = th_resolve_c(b);
  if (a == b)
    return true;
  if (!a || !b || a->kind != b->kind)
    return false;
  if (pair_seen(seen, a, b))
    return true;
  switch (a->kind) {
  case TH_VAR:
    return false;
  case TH_NAMED:
    if (a->named.is_variant != b->named.is_variant ||
        a->named.nlen != b->named.nlen ||
        memcmp(a->named.name, b->named.name, a->named.nlen) ||
        a->named.nargs != b->named.nargs)
      return false;
    for (size_t i = 0; i < a->named.nargs; i++) {
      const th_arg *x = &a->named.args[i], *y = &b->named.args[i];
      if (x->kind != y->kind)
        return false;
      if (x->kind == TH_ARG_TYPE) {
        if (!pattern_equal(x->type, y->type, seen))
          return false;
      } else if (x->kind == TH_ARG_UINT && x->u != y->u)
        return false;
      else if (x->kind == TH_ARG_SINT && x->s != y->s)
        return false;
      else if (x->kind == TH_ARG_STR &&
               (x->str.n != y->str.n || memcmp(x->str.p, y->str.p, x->str.n)))
        return false;
    }
    return true;
  case TH_BITINT:
    return a->bitint.width == b->bitint.width &&
           a->bitint.flavor == b->bitint.flavor;
  case TH_INDIRECTION:
    return a->ind.attrs == b->ind.attrs &&
           pattern_equal(a->ind.referent, b->ind.referent, seen);
  case TH_SEQUENCE:
    return a->seq.has_extent == b->seq.has_extent &&
           (!a->seq.has_extent || a->seq.extent == b->seq.extent) &&
           pattern_equal(a->seq.elem, b->seq.elem, seen);
  case TH_RECORD:
    if (a->record.n != b->record.n)
      return false;
    for (size_t i = 0; i < a->record.n; i++) {
      const th_member *x = &a->record.members[i], *y = &b->record.members[i];
      if (strcmp(x->name, y->name) || !pattern_equal(x->type, y->type, seen))
        return false;
    }
    return true;
  case TH_FUNCTION:
    if (a->fn.np != b->fn.np || a->fn.nkw != b->fn.nkw ||
        !!a->fn.rest != !!b->fn.rest ||
        !!a->fn.error != !!b->fn.error ||
        !pattern_equal(a->fn.ret, b->fn.ret, seen) ||
        (a->fn.error && !pattern_equal(a->fn.error, b->fn.error, seen)))
      return false;
    for (size_t i = 0; i < a->fn.np; i++)
      if (a->fn.params[i].mode != b->fn.params[i].mode ||
          !pattern_equal(a->fn.params[i].type, b->fn.params[i].type, seen))
        return false;
    if (a->fn.rest &&
        (a->fn.rest->mode != b->fn.rest->mode ||
         !pattern_equal(a->fn.rest->type, b->fn.rest->type, seen)))
      return false;
    for (size_t i = 0; i < a->fn.nkw; i++)
      if (strcmp(a->fn.kw[i].name, b->fn.kw[i].name) ||
          a->fn.kw[i].mode != b->fn.kw[i].mode ||
          !pattern_equal(a->fn.kw[i].type, b->fn.kw[i].type, seen))
        return false;
    return true;
  case TH_BINDER:
    return a->binder.arity == b->binder.arity &&
           pattern_equal(a->binder.body, b->binder.body, seen);
  case TH_BOUND:
    return a->bound.distance == b->bound.distance &&
           a->bound.position == b->bound.position;
  case TH_SUM:
    if (a->sum.comb != b->sum.comb || a->sum.n != b->sum.n)
      return false;
    for (size_t i = 0; i < a->sum.n; i++)
      if (!pattern_equal(a->sum.arms[i], b->sum.arms[i], seen))
        return false;
    return true;
  }
  return false;
}

static bool pattern_subsumes(const th_type *general, const th_type *specific,
                             core_list_t(type_binding_t) * bindings,
                             core_list_t(type_pair_t) * seen) {
  general = th_resolve_c(general);
  specific = th_resolve_c(specific);
  if (!general || !specific)
    return false;
  if (general->kind == TH_VAR) {
    for (size_t i = 0; i < bindings->len; i++)
      if (bindings->data[i].variable == general) {
        core_list_t(type_pair_t) equality = core_list_new(type_pair_t);
        bool same = pattern_equal(bindings->data[i].value, specific, &equality);
        core_list_free(equality);
        return same;
      }
    core_list_push(*bindings, ((type_binding_t){general, specific}));
    return true;
  }
  if (general->kind != specific->kind)
    return false;
  if (pair_seen(seen, general, specific))
    return true;
  switch (general->kind) {
  case TH_VAR:
    return true;
  case TH_NAMED:
    if (general->named.is_variant != specific->named.is_variant ||
        general->named.nlen != specific->named.nlen ||
        memcmp(general->named.name, specific->named.name,
               general->named.nlen) ||
        general->named.nargs != specific->named.nargs)
      return false;
    for (size_t i = 0; i < general->named.nargs; i++) {
      const th_arg *x = &general->named.args[i], *y = &specific->named.args[i];
      if (x->kind != y->kind)
        return false;
      if (x->kind == TH_ARG_TYPE) {
        if (!pattern_subsumes(x->type, y->type, bindings, seen))
          return false;
      } else if (x->kind == TH_ARG_UINT && x->u != y->u)
        return false;
      else if (x->kind == TH_ARG_SINT && x->s != y->s)
        return false;
      else if (x->kind == TH_ARG_STR &&
               (x->str.n != y->str.n || memcmp(x->str.p, y->str.p, x->str.n)))
        return false;
    }
    return true;
  case TH_BITINT:
    return general->bitint.width == specific->bitint.width &&
           general->bitint.flavor == specific->bitint.flavor;
  case TH_INDIRECTION:
    return general->ind.attrs == specific->ind.attrs &&
           pattern_subsumes(general->ind.referent, specific->ind.referent,
                            bindings, seen);
  case TH_SEQUENCE:
    return general->seq.has_extent == specific->seq.has_extent &&
           (!general->seq.has_extent ||
            general->seq.extent == specific->seq.extent) &&
           pattern_subsumes(general->seq.elem, specific->seq.elem, bindings,
                            seen);
  case TH_RECORD:
    if (general->record.n != specific->record.n)
      return false;
    for (size_t i = 0; i < general->record.n; i++)
      if (strcmp(general->record.members[i].name,
                 specific->record.members[i].name) ||
          !pattern_subsumes(general->record.members[i].type,
                            specific->record.members[i].type, bindings, seen))
        return false;
    return true;
  case TH_FUNCTION:
    if (general->fn.np != specific->fn.np ||
        general->fn.nkw != specific->fn.nkw ||
        !!general->fn.rest != !!specific->fn.rest ||
        !!general->fn.error != !!specific->fn.error ||
        !pattern_subsumes(general->fn.ret, specific->fn.ret, bindings, seen))
      return false;
    if (general->fn.error &&
        !pattern_subsumes(general->fn.error, specific->fn.error, bindings,
                          seen))
      return false;
    for (size_t i = 0; i < general->fn.np; i++)
      if (general->fn.params[i].mode != specific->fn.params[i].mode ||
          !pattern_subsumes(general->fn.params[i].type,
                            specific->fn.params[i].type, bindings, seen))
        return false;
    if (general->fn.rest &&
        (general->fn.rest->mode != specific->fn.rest->mode ||
         !pattern_subsumes(general->fn.rest->type, specific->fn.rest->type,
                           bindings, seen)))
      return false;
    for (size_t i = 0; i < general->fn.nkw; i++)
      if (strcmp(general->fn.kw[i].name, specific->fn.kw[i].name) ||
          general->fn.kw[i].mode != specific->fn.kw[i].mode ||
          !pattern_subsumes(general->fn.kw[i].type, specific->fn.kw[i].type,
                            bindings, seen))
        return false;
    return true;
  case TH_BINDER:
    return general->binder.arity == specific->binder.arity &&
           pattern_subsumes(general->binder.body, specific->binder.body,
                            bindings, seen);
  case TH_BOUND:
    return general->bound.distance == specific->bound.distance &&
           general->bound.position == specific->bound.position;
  case TH_SUM:
    if (general->sum.comb != specific->sum.comb ||
        general->sum.n != specific->sum.n)
      return false;
    for (size_t i = 0; i < general->sum.n; i++)
      if (!pattern_subsumes(general->sum.arms[i], specific->sum.arms[i],
                            bindings, seen))
        return false;
    return true;
  }
  return false;
}

static bool receiver_subsumes(th_type *general, th_type *specific) {
  core_list_t(type_binding_t) bindings = core_list_new(type_binding_t);
  core_list_t(type_pair_t) seen = core_list_new(type_pair_t);
  bool result = pattern_subsumes(general, specific, &bindings, &seen);
  core_list_free(bindings);
  core_list_free(seen);
  return result;
}

bool th_env_type_pattern_subsumes(const th_type *general,
                                  const th_type *specific) {
  return general && specific && receiver_subsumes((th_type *)general,
                                                   (th_type *)specific);
}

th_env *th_env_new(void) {
  th_env *env = core_alloc(th_env);
  memset(env, 0, sizeof(*env));
  env->types = th_ctx_new();
  env->owns_types = true;
  env->traits = core_list_new(th_env_trait_t);
  env->functions = core_list_new(th_env_function_t);
  env->values = core_list_new(th_env_value_t);
  env->intrinsic_types = core_list_new(th_env_intrinsic_type_t);
  env->type_origins = core_list_new(th_env_type_origin_t);
  env->method_symbols = core_list_new(th_env_method_symbol_t);
  env->metadata = core_list_new(th_env_metadata_record_t);
  env->logic = core_datalog_engine_new();
  env->candidate_relation = core_datalog_relation(
      env->logic, CORE_STRING_STATIC("trait_method_candidate"), 3);
  env->intrinsic_type_relation = core_datalog_relation(
      env->logic, CORE_STRING_STATIC("intrinsic_trait_type"), 2);
  return env;
}

th_env *th_env_new_in_context(th_ctx *types) {
  if (!types) return nullptr;
  th_env *env = th_env_new();
  if (!env) return nullptr;
  th_ctx_free(env->types);
  env->types = types;
  env->owns_types = false;
  return env;
}

static th_env *th_env_compose_diagnose_in_context(
    const th_env *const *environments, size_t count, th_ctx *types,
    th_env_compose_error_t *error);

th_env *th_env_compose(const th_env *const *environments, size_t count) {
  return th_env_compose_diagnose(environments, count, nullptr);
}

static void report_compose_error(core_diagnostics_t *diagnostics,
                                 const th_env_compose_error_t *error) {
  if (!diagnostics || !error || error->status == TH_ENV_COMPOSE_OK)
    return;
  const char *code = "TH-L0001";
  const char *kind = "invalid source environment";
  switch (error->status) {
  case TH_ENV_COMPOSE_OK:
    return;
  case TH_ENV_COMPOSE_INVALID_SOURCE:
    break;
  case TH_ENV_COMPOSE_TYPE_CONFLICT:
    code = "TH-L0002";
    kind = "conflicting type declaration";
    break;
  case TH_ENV_COMPOSE_TRAIT_CONFLICT:
    code = "TH-L0003";
    kind = "conflicting trait declaration";
    break;
  case TH_ENV_COMPOSE_FUNCTION_CONFLICT:
    code = "TH-L0004";
    kind = "conflicting function declaration";
    break;
  case TH_ENV_COMPOSE_METADATA_CONFLICT:
    code = "TH-L0005";
    kind = "conflicting metadata";
    break;
  case TH_ENV_COMPOSE_VALUE_CONFLICT:
    code = "TH-L0006";
    kind = "conflicting value declaration";
    break;
  }
  char message[512];
  int length = error->identity
                   ? snprintf(message, sizeof(message), "%s: %s", kind,
                              error->identity)
                   : snprintf(message, sizeof(message), "%s", kind);
  if (error->metadata_key && length > 0 &&
      (size_t)length < sizeof(message)) {
    int extra = snprintf(message + length, sizeof(message) - (size_t)length,
                         " (key %s)", error->metadata_key);
    if (extra > 0)
      length += extra;
  }
  if (length < 0)
    return;
  size_t size = (size_t)length;
  if (size >= sizeof(message))
    size = sizeof(message) - 1;
  core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_ERROR,
                          core_string_borrow(code, strlen(code)),
                          core_string_borrow(message, size));
}

th_env *th_env_compose_report(const th_env *const *environments, size_t count,
                              core_diagnostics_t *diagnostics,
                              th_env_compose_error_t *error) {
  return th_env_compose_report_in_context(environments, count, nullptr,
                                          diagnostics, error);
}

th_env *th_env_compose_report_in_context(
    const th_env *const *environments, size_t count, th_ctx *types,
    core_diagnostics_t *diagnostics, th_env_compose_error_t *error) {
  th_env_compose_error_t local_error = {0};
  th_env *result = th_env_compose_diagnose_in_context(
      environments, count, types, error ? error : &local_error);
  report_compose_error(diagnostics, error ? error : &local_error);
  return result;
}

th_env *th_env_compose_diagnose(const th_env *const *environments,
                                size_t count, th_env_compose_error_t *error) {
  return th_env_compose_diagnose_in_context(environments, count, nullptr,
                                            error);
}

static th_type *
composition_type(const th_env *source, th_env *destination,
                 const th_type *type)
{
  if (!type) return nullptr;
  return source->types == destination->types
      ? (th_type *)type : th_type_clone(destination->types, type);
}

static th_env *
th_env_compose_diagnose_in_context(const th_env *const *environments,
                                   size_t count, th_ctx *types,
                                   th_env_compose_error_t *error)
{
  if (error)
    *error = (th_env_compose_error_t){.status = TH_ENV_COMPOSE_OK};
  if (count && !environments)
    goto invalid_source;

  th_env *composed = types ? th_env_new_in_context(types) : th_env_new();
  for (size_t i = 0; i < count; i++) {
    const th_env *source = environments[i];
    if (!source || !source->frozen)
      goto invalid_source_with_env;

    for (size_t j = 0; j < source->type_origins.len; j++) {
      const th_env_type_origin_t *origin = &source->type_origins.data[j];
      if (!th_env_register_type(composed, origin->name, origin->module,
                                origin->package, origin->visibility)) {
        if (error)
          *error = (th_env_compose_error_t){
              .status = TH_ENV_COMPOSE_TYPE_CONFLICT,
              .identity = origin->name};
        goto fail;
      }
      if (origin->shape_kind != TH_ENV_TYPE_SHAPE_NONE &&
          !th_env_register_type_shape(composed, origin->name,
              origin->shape_kind,
              composition_type(source, composed, origin->shape))) {
        if (error)
          *error = (th_env_compose_error_t){
              .status = TH_ENV_COMPOSE_TYPE_CONFLICT,
              .identity = origin->name};
        goto fail;
      }
    }
    for (size_t j = 0; j < source->traits.len; j++) {
      const th_env_trait_t *trait = &source->traits.data[j];
      th_env_trait_t *existing = trait_named(composed, trait->name);
      bool merge_marker = existing &&
          th_record_len(existing->contracts) == 0 &&
          th_record_len(trait->contracts) == 0 &&
          existing->visibility == trait->visibility &&
          !strcmp(existing->module, trait->module) &&
          !strcmp(existing->package, trait->package);
      if (!merge_marker && !th_env_register_trait_contracts(
              composed, trait->name, trait->module, trait->package,
              trait->visibility,
              composition_type(source, composed, trait->contracts))) {
        if (error)
          *error = (th_env_compose_error_t){
              .status = TH_ENV_COMPOSE_TRAIT_CONFLICT,
              .identity = trait->name};
        goto fail;
      }
    }
    for (size_t j = 0; j < source->functions.len; j++) {
      const th_env_function_t *function = &source->functions.data[j];
      if (!th_env_register_function_contract(
              composed, function->qualified_name, function->root_name,
              function->module, function->package, function->visibility,
              composition_type(source, composed, function->contract))) {
        if (error)
          *error = (th_env_compose_error_t){
              .status = TH_ENV_COMPOSE_FUNCTION_CONFLICT,
              .identity = function->qualified_name};
        goto fail;
      }
    }
    for (size_t j = 0; j < source->values.len; j++) {
      const th_env_value_t *value = &source->values.data[j];
      if (!th_env_register_value(composed, value->qualified_name,
              value->root_name, value->module, value->package,
              value->visibility,
              composition_type(source, composed, value->type))) {
        if (error)
          *error = (th_env_compose_error_t){
              .status = TH_ENV_COMPOSE_VALUE_CONFLICT,
              .identity = value->qualified_name};
        goto fail;
      }
    }
    for (size_t j = 0; j < source->intrinsic_types.len; j++) {
      const th_env_intrinsic_type_t *fact =
          &source->intrinsic_types.data[j];
      if (!th_env_register_intrinsic_type_pattern(
              composed, fact->trait_name,
              composition_type(source, composed,
                              fact->receiver_pattern))) {
        if (error)
          *error = (th_env_compose_error_t){
              .status = TH_ENV_COMPOSE_TRAIT_CONFLICT,
              .identity = fact->trait_name};
        goto fail;
      }
    }
    for (size_t j = 0; j < source->metadata.len; j++) {
      const th_env_metadata_record_t *metadata = &source->metadata.data[j];
      if (!th_env_register_metadata(
              composed, metadata->owner_kind, metadata->owner_name,
              metadata->key, metadata->version, metadata->data,
              metadata->size)) {
        if (error)
          *error = (th_env_compose_error_t){
              .status = TH_ENV_COMPOSE_METADATA_CONFLICT,
              .identity = metadata->owner_name,
              .metadata_key = metadata->key};
        goto fail;
      }
    }
  }
  return composed;

invalid_source_with_env:
  if (error)
    *error = (th_env_compose_error_t){
        .status = TH_ENV_COMPOSE_INVALID_SOURCE};
  th_env_free(composed);
  return nullptr;

fail:
  th_env_free(composed);
  return nullptr;

invalid_source:
  if (error)
    *error = (th_env_compose_error_t){
        .status = TH_ENV_COMPOSE_INVALID_SOURCE};
  return nullptr;
}

void th_env_release_contents(th_env *env) {
  core_list_free(env->traits);
  core_list_free(env->functions);
  core_list_free(env->values);
  core_list_free(env->intrinsic_types);
  core_list_free(env->type_origins);
  core_list_free(env->method_symbols);
  core_list_free(env->metadata);
  core_datalog_engine_free(env->logic);
  env->logic = nullptr;
  if (env->owns_types)
    th_ctx_free(env->types);
  env->types = nullptr;
}

void th_env_free(th_env *env) {
  if (!env)
    return;
  if (env->persisted_archive) {
    core_object_archive_close(env->persisted_archive, nullptr);
    return;
  }
  th_env_release_contents(env);
  core_free(env);
}

bool th_env_register_type(th_env *env, const char *name, const char *module,
                          const char *package,
                          th_env_visibility_t visibility) {
  if (!env || env->frozen || !name || !*name || !module || !package)
    return false;
  if (visibility > TH_ENV_PUBLIC)
    return false;
  for (size_t i = 0; i < env->type_origins.len; i++) {
    th_env_type_origin_t *origin = &env->type_origins.data[i];
    if (strcmp(origin->name, name))
      continue;
    return !strcmp(origin->module, module) &&
           !strcmp(origin->package, package) &&
           origin->visibility == visibility;
  }
  core_list_push(env->type_origins,
                 ((th_env_type_origin_t){
                     .name = th_strdup(env->types, name, strlen(name)),
                     .module = th_strdup(env->types, module, strlen(module)),
                     .package = th_strdup(env->types, package, strlen(package)),
                     .visibility = visibility,
                 }));
  return true;
}

bool th_env_register_type_shape(th_env *env, const char *name,
                                th_env_type_shape_kind_t shape_kind,
                                th_type *shape) {
  if (!env || env->frozen || !name || !*name || !shape ||
      shape_kind <= TH_ENV_TYPE_SHAPE_NONE ||
      shape_kind > TH_ENV_TYPE_SHAPE_BITSET)
    return false;
  for (size_t i = 0; i < env->type_origins.len; i++) {
    th_env_type_origin_t *origin = &env->type_origins.data[i];
    if (strcmp(origin->name, name)) continue;
    if (origin->shape_kind == TH_ENV_TYPE_SHAPE_NONE) {
      origin->shape_kind = shape_kind;
      origin->shape = shape;
      return true;
    }
    if (origin->shape_kind != shape_kind) return false;
    core_list_t(type_pair_t) seen = core_list_new(type_pair_t);
    bool equal = pattern_equal(origin->shape, shape, &seen);
    core_list_free(seen);
    return equal;
  }
  return false;
}

bool th_env_register_trait(th_env *env, const char *name, const char *module,
                           const char *package, th_env_visibility_t visibility,
                           th_type *methods) {
  return th_env_register_trait_channels(env, name, module, package,
                                        visibility, methods, nullptr);
}

static th_type *fresh_pattern(th_ctx *ctx, th_type *type) {
  if (!ctx || !type) return nullptr;
  return th_kind_of(th_resolve(type)) == TH_BINDER
      ? th_instantiate(ctx, type)
      : th_type_clone(ctx, type);
}

th_type *th_env_signature_contract(th_ctx *ctx, th_type *signature,
                                   th_type *error_type) {
  if (!ctx || !signature || !error_type) return nullptr;
  if (th_kind_of(signature) == TH_BINDER)
    signature = th_instantiate(ctx, signature);
  if (th_kind_of(error_type) == TH_BINDER)
    error_type = th_instantiate(ctx, error_type);
  if (!signature || th_kind_of(signature) != TH_FUNCTION || !error_type)
    return nullptr;
  if (signature->fn.error &&
      !th_unify(ctx, signature->fn.error, error_type)) return nullptr;
  th_type *failure = signature->fn.error ? signature->fn.error : error_type;
  th_type *paired = th_func_with_error(
      ctx, signature->fn.ret, failure, signature->fn.params,
      signature->fn.np, signature->fn.rest, signature->fn.kw,
      signature->fn.nkw);
  return paired;
}

static bool signature_channels_raw(th_ctx *ctx, th_type *contract,
                                   th_type **signature,
                                   th_type **error_type) {
  if (signature) *signature = nullptr;
  if (error_type) *error_type = nullptr;
  if (!contract) return false;
  if (th_kind_of(contract) == TH_BINDER)
    contract = th_instantiate(ctx, contract);
  if (!contract || th_kind_of(contract) != TH_FUNCTION) return false;
  if (signature) *signature = contract;
  if (error_type)
    *error_type = contract->fn.error ? contract->fn.error
                                     : th_name(ctx, "nil");
  return true;
}

bool th_env_signature_channels(th_ctx *ctx, th_type *contract,
                               th_type **signature, th_type **error_type) {
  if (signature) *signature = nullptr;
  if (error_type) *error_type = nullptr;
  return ctx && contract && signature_channels_raw(
      ctx, fresh_pattern(ctx, contract), signature, error_type);
}

bool th_env_register_trait_channels(th_env *env, const char *name,
                                    const char *module, const char *package,
                                    th_env_visibility_t visibility,
                                    th_type *methods, th_type *errors) {
  if (!env || env->frozen || !name || !*name || !module || !package ||
      !methods || visibility > TH_ENV_PUBLIC ||
      th_kind_of(methods) != TH_RECORD || !th_record_len(methods) ||
      trait_named(env, name))
    return false;
  core_list_t(th_member) default_errors = core_list_new(th_member);
  if (!errors) {
    for (size_t i = 0; i < th_record_len(methods); i++) {
      const th_member *member = th_record_member(methods, i);
      core_list_push(default_errors, ((th_member){
          .name = member->name,
          .type = th_name(env->types, "nil"),
      }));
    }
    errors = th_record(env->types, default_errors.data, default_errors.len);
  }
  if (!errors || th_kind_of(errors) != TH_RECORD ||
      th_record_len(errors) != th_record_len(methods)) {
    core_list_free(default_errors);
    return false;
  }
  core_list_t(th_member) contracts = core_list_new(th_member);
  for (size_t i = 0; i < th_record_len(methods); i++) {
    const th_member *member = th_record_member(methods, i);
    th_type *error = member ? th_record_lookup(errors, member->name) : nullptr;
    th_type *contract = member && error
        ? th_env_signature_contract(env->types, member->type, error) : nullptr;
    th_type *signature = nullptr;
    if (contract)
      th_env_signature_channels(env->types, contract, &signature, nullptr);
    th_type *receiver = first_parameter(signature);
    if (!receiver || !error ||
        th_resolve(receiver)->kind != TH_VAR ||
        th_resolve(receiver)->var.ref)
    {
      core_list_free(contracts);
      core_list_free(default_errors);
      return false;
    }
    core_list_push(contracts,
        ((th_member){.name = member->name, .type = contract}));
  }
  th_type *contract_record =
      th_record(env->types, contracts.data, contracts.len);
  bool registered = th_env_register_trait_contracts(
      env, name, module, package, visibility, contract_record);
  core_list_free(contracts);
  core_list_free(default_errors);
  return registered;
}

bool th_env_register_trait_contracts(th_env *env, const char *name,
                                     const char *module, const char *package,
                                     th_env_visibility_t visibility,
                                     th_type *method_contracts) {
  if (!env || env->frozen || !name || !*name || !module || !package ||
      !method_contracts || visibility > TH_ENV_PUBLIC ||
      th_kind_of(method_contracts) != TH_RECORD || trait_named(env, name))
    return false;
  core_list_t(th_member) methods = core_list_new(th_member);
  core_list_t(th_member) errors = core_list_new(th_member);
  for (size_t i = 0; i < th_record_len(method_contracts); i++) {
    const th_member *member = th_record_member(method_contracts, i);
    th_type *signature = nullptr, *error = nullptr;
    bool valid = member && th_env_signature_channels(
        env->types, member->type, &signature, &error);
    th_type *receiver = valid ? first_parameter(signature) : nullptr;
    if (!receiver || th_kind_of(th_resolve(receiver)) != TH_VAR ||
        th_resolve(receiver)->var.ref) {
      core_list_free(methods);
      core_list_free(errors);
      return false;
    }
    core_list_push(methods, ((th_member){.name = member->name,
                                         .type = signature}));
    core_list_push(errors, ((th_member){.name = member->name,
                                        .type = error}));
  }
  core_list_push(env->traits, ((th_env_trait_t){
      .name = th_strdup(env->types, name, strlen(name)),
      .module = th_strdup(env->types, module, strlen(module)),
      .package = th_strdup(env->types, package, strlen(package)),
      .visibility = visibility,
      .scheme = th_record(env->types, methods.data, methods.len),
      .errors = th_record(env->types, errors.data, errors.len),
      .contracts = method_contracts,
  }));
  core_list_free(methods);
  core_list_free(errors);
  return true;
}

bool th_env_register_intrinsic_type_pattern(th_env *env,
                                           const char *trait_name,
                                           th_type *receiver_pattern) {
  th_env_trait_t *trait = env && trait_name
      ? trait_named(env, trait_name) : nullptr;
  if (!env || env->frozen || !trait || !receiver_pattern ||
      th_record_len(trait->contracts) != 0)
    return false;
  for (size_t i = 0; i < env->intrinsic_types.len; i++) {
    const th_env_intrinsic_type_t *fact = &env->intrinsic_types.data[i];
    if (!strcmp(fact->trait_name, trait_name) &&
        fact->receiver_pattern == receiver_pattern)
      return false;
  }
  core_list_push(env->intrinsic_types,
      ((th_env_intrinsic_type_t){
          .trait_name = trait->name,
          .receiver_pattern = receiver_pattern,
      }));
  return true;
}

bool th_env_register_function(th_env *env, const char *qualified_name,
                              const char *root_name, const char *module,
                              const char *package,
                              th_env_visibility_t visibility,
                              th_type *signature) {
  return th_env_register_function_channels(
      env, qualified_name, root_name, module, package, visibility, signature,
      env ? th_name(env->types, "nil") : nullptr);
}

bool th_env_register_function_channels(
    th_env *env, const char *qualified_name, const char *root_name,
    const char *module, const char *package,
    th_env_visibility_t visibility, th_type *signature, th_type *error_type) {
  if (!env || !signature || !error_type) return false;
  return th_env_register_function_contract(
      env, qualified_name, root_name, module, package, visibility,
      th_env_signature_contract(env->types, signature, error_type));
}

bool th_env_register_function_contract(
    th_env *env, const char *qualified_name, const char *root_name,
    const char *module, const char *package,
    th_env_visibility_t visibility, th_type *contract) {
  if (!env || env->frozen || !qualified_name || !*qualified_name ||
      !root_name || !*root_name || !module || !package || !contract ||
      visibility > TH_ENV_PUBLIC)
    return false;
  th_type *signature = nullptr, *error_type = nullptr;
  if (!th_env_signature_channels(env->types, contract, &signature,
                                 &error_type)) return false;
  for (size_t i = 0; i < env->functions.len; i++)
    if (!strcmp(env->functions.data[i].qualified_name, qualified_name))
      return false;
  core_list_push(
      env->functions,
      ((th_env_function_t){
          .qualified_name =
              th_strdup(env->types, qualified_name, strlen(qualified_name)),
          .root_name = th_strdup(env->types, root_name, strlen(root_name)),
          .module = th_strdup(env->types, module, strlen(module)),
          .package = th_strdup(env->types, package, strlen(package)),
          .visibility = visibility,
          .order = env->functions.len,
          .scheme = signature,
          .error_type = error_type,
          .contract = contract,
      }));
  return true;
}

bool th_env_register_value(th_env *env, const char *qualified_name,
                           const char *root_name, const char *module,
                           const char *package, th_env_visibility_t visibility,
                           th_type *value_type) {
  if (!env || env->frozen || !qualified_name || !*qualified_name ||
      !root_name || !*root_name || !module || !package || !value_type ||
      visibility > TH_ENV_PUBLIC) return false;
  for (size_t i = 0; i < env->values.len; i++) {
    const th_env_value_t *current = &env->values.data[i];
    if (strcmp(current->qualified_name, qualified_name)) continue;
    return !strcmp(current->module, module) &&
        !strcmp(current->package, package) &&
        current->visibility == visibility &&
        th_hash(env->types, current->type) == th_hash(env->types, value_type);
  }
  core_list_push(env->values, ((th_env_value_t){
      .qualified_name = th_strdup(env->types, qualified_name,
                                  strlen(qualified_name)),
      .root_name = th_strdup(env->types, root_name, strlen(root_name)),
      .module = th_strdup(env->types, module, strlen(module)),
      .package = th_strdup(env->types, package, strlen(package)),
      .visibility = visibility,
      .type = value_type,
  }));
  return true;
}

static bool metadata_owner_exists(const th_env *env,
                                  th_env_metadata_owner_t owner_kind,
                                  const char *owner_name) {
  switch (owner_kind) {
  case TH_ENV_METADATA_TYPE:
    for (size_t i = 0; i < env->type_origins.len; i++)
      if (!strcmp(env->type_origins.data[i].name, owner_name))
        return true;
    return false;
  case TH_ENV_METADATA_TRAIT:
    return trait_named(env, owner_name) != nullptr;
  case TH_ENV_METADATA_FUNCTION:
    for (size_t i = 0; i < env->functions.len; i++)
      if (!strcmp(env->functions.data[i].qualified_name, owner_name))
        return true;
    return false;
  case TH_ENV_METADATA_VALUE:
    for (size_t i = 0; i < env->values.len; i++)
      if (!strcmp(env->values.data[i].qualified_name, owner_name))
        return true;
    return false;
  case TH_ENV_METADATA_ENVIRONMENT:
    return true;
  }
  return false;
}

bool th_env_register_metadata(th_env *env, th_env_metadata_owner_t owner_kind,
                              const char *owner_name, const char *key,
                              uint32_t version, const void *data,
                              size_t size) {
  if (!env || env->frozen || owner_kind < TH_ENV_METADATA_TYPE ||
      owner_kind > TH_ENV_METADATA_ENVIRONMENT ||
      !owner_name || !*owner_name || !key || !*key || !version ||
      (size && !data) || !metadata_owner_exists(env, owner_kind, owner_name))
    return false;
  for (size_t i = 0; i < env->metadata.len; i++) {
    const th_env_metadata_record_t *current = &env->metadata.data[i];
    if (current->owner_kind == owner_kind &&
        !strcmp(current->owner_name, owner_name) && !strcmp(current->key, key))
      return false;
  }

  uint8_t *owned_data = size ? th_alloc(env->types, size) : nullptr;
  if (size)
    memcpy(owned_data, data, size);
  core_list_push(env->metadata,
                 ((th_env_metadata_record_t){
                     .owner_kind = owner_kind,
                     .owner_name = th_strdup(env->types, owner_name,
                                             strlen(owner_name)),
                     .key = th_strdup(env->types, key, strlen(key)),
                     .version = version,
                     .data = owned_data,
                     .size = size,
                 }));
  return true;
}

size_t th_env_metadata_count(const th_env *env) {
  return env ? env->metadata.len : 0;
}

bool th_env_metadata_at(const th_env *env, size_t index,
                        th_env_metadata_view_t *out) {
  if (!env || !out || index >= env->metadata.len)
    return false;
  const th_env_metadata_record_t *entry = &env->metadata.data[index];
  *out = (th_env_metadata_view_t){
      .owner_kind = entry->owner_kind,
      .owner_name = entry->owner_name,
      .key = entry->key,
      .version = entry->version,
      .data = entry->data,
      .size = entry->size,
  };
  return true;
}

size_t th_env_declaration_count(const th_env *env) {
  if (!env || env->type_origins.len > SIZE_MAX - env->traits.len ||
      env->type_origins.len + env->traits.len >
          SIZE_MAX - env->functions.len ||
      env->type_origins.len + env->traits.len + env->functions.len >
          SIZE_MAX - env->values.len)
    return 0;
  return env->type_origins.len + env->traits.len + env->functions.len +
      env->values.len;
}

bool th_env_declaration_at(const th_env *env, size_t index,
                           th_env_declaration_view_t *out) {
  if (!env || !out) return false;
  if (index < env->type_origins.len) {
    const th_env_type_origin_t *origin = &env->type_origins.data[index];
    *out = (th_env_declaration_view_t){
        .kind = TH_ENV_DECLARATION_TYPE,
        .qualified_name = origin->name,
        .module = origin->module,
        .package = origin->package,
        .visibility = origin->visibility,
        .type_shape_kind = origin->shape_kind,
        .type_shape = origin->shape,
    };
    return true;
  }
  index -= env->type_origins.len;
  if (index < env->traits.len) {
    const th_env_trait_t *trait = &env->traits.data[index];
    *out = (th_env_declaration_view_t){
        .kind = TH_ENV_DECLARATION_TRAIT,
        .qualified_name = trait->name,
        .module = trait->module,
        .package = trait->package,
        .visibility = trait->visibility,
        .contract = trait->contracts,
    };
    return true;
  }
  index -= env->traits.len;
  if (index < env->functions.len) {
    const th_env_function_t *function = &env->functions.data[index];
    *out = (th_env_declaration_view_t){
        .kind = TH_ENV_DECLARATION_FUNCTION,
        .qualified_name = function->qualified_name,
        .root_name = function->root_name,
        .module = function->module,
        .package = function->package,
        .visibility = function->visibility,
        .contract = function->contract,
    };
    return true;
  }
  index -= env->functions.len;
  if (index < env->values.len) {
    const th_env_value_t *value = &env->values.data[index];
    *out = (th_env_declaration_view_t){
        .kind = TH_ENV_DECLARATION_VALUE,
        .qualified_name = value->qualified_name,
        .root_name = value->root_name,
        .module = value->module,
        .package = value->package,
        .visibility = value->visibility,
        .value_type = value->type,
    };
    return true;
  }
  return false;
}

bool th_env_freeze(th_env *env) {
  if (!env || !env->logic || env->candidate_relation < 0 ||
      env->intrinsic_type_relation < 0)
    return false;
  if (env->frozen)
    return true;
  th_ctx *probe = th_ctx_new();
  probe->environment = env;
  for (size_t i = 0; i < env->functions.len; i++) {
    env->functions.data[i].logic_symbol =
        core_datalog_integer(env->logic, (int64_t)i);
    if (env->functions.data[i].logic_symbol == CORE_DATALOG_SYMBOL_INVALID) {
      th_ctx_free(probe);
      return false;
    }
  }
  /* Datalog stores only stable candidate identities; substitutions are
   * intentionally recomputed by the type layer for each receiver. */
  for (size_t ti = 0; ti < env->traits.len; ti++) {
    th_env_trait_t *trait = &env->traits.data[ti];
    trait->logic_symbol = core_datalog_constant(
        env->logic, core_string_borrow(trait->name, strlen(trait->name)));
    th_type *methods = fresh_pattern(probe, trait->contracts);
    for (size_t mi = 0; mi < th_record_len(methods); mi++) {
      const th_member *method = th_record_member(methods, mi);
      const th_member *stable_method = th_record_member(trait->contracts, mi);
      core_datalog_symbol_t method_symbol = core_datalog_constant(
          env->logic, core_string_borrow(method->name, strlen(method->name)));
      core_list_push(
          env->method_symbols,
          ((th_env_method_symbol_t){ti, stable_method->name, method_symbol}));
      for (size_t fi = 0; fi < env->functions.len; fi++) {
        th_env_function_t *function = &env->functions.data[fi];
        if (strcmp(function->root_name, method->name) ||
            !function_can_match(probe, trait, function, method->name))
          continue;
        core_datalog_symbol_t tuple[] = {trait->logic_symbol, method_symbol,
                                         function->logic_symbol};
        if (!core_datalog_add_fact(env->logic, env->candidate_relation, 3,
                                   tuple)) {
          th_ctx_free(probe);
          return false;
        }
      }
    }
  }
  for (size_t i = 0; i < env->intrinsic_types.len; i++) {
    th_env_intrinsic_type_t *fact = &env->intrinsic_types.data[i];
    th_env_trait_t *trait = trait_named(env, fact->trait_name);
    fact->logic_symbol = core_datalog_integer(env->logic, (int64_t)i);
    core_datalog_symbol_t tuple[] = {
        trait ? trait->logic_symbol : CORE_DATALOG_SYMBOL_INVALID,
        fact->logic_symbol,
    };
    if (!trait || fact->logic_symbol == CORE_DATALOG_SYMBOL_INVALID ||
        !core_datalog_add_fact(env->logic, env->intrinsic_type_relation, 2,
                               tuple)) {
      th_ctx_free(probe);
      return false;
    }
  }
  th_ctx_free(probe);
  if (!core_datalog_run(env->logic))
    return false;
  env->frozen = true;
  return true;
}

bool th_env_is_frozen(const th_env *env) { return env && env->frozen; }

bool th_ctx_set_environment(th_ctx *ctx, const th_env *env, const char *module,
                            const char *package) {
  if (!ctx || !env || !env->frozen)
    return false;
  module = module ? module : "";
  package = package ? package : "";
  if (ctx->environment)
    return ctx->environment == env && ctx->module && ctx->package &&
           !strcmp(ctx->module, module) && !strcmp(ctx->package, package);
  ctx->environment = env;
  if (!th_ctx_set_scope(ctx, module, package)) return false;
  return th_ctx_validate_constraints(ctx);
}

bool th_ctx_set_scope(th_ctx *ctx, const char *module, const char *package) {
  if (!ctx) return false;
  module = module ? module : "";
  package = package ? package : "";
  if (!ctx->module || strcmp(ctx->module, module))
    ctx->module = th_strdup(ctx, module, strlen(module));
  if (!ctx->package || strcmp(ctx->package, package))
    ctx->package = th_strdup(ctx, package, strlen(package));
  return true;
}

bool th_ctx_set_environment_scope(th_ctx *ctx, const char *module,
                                  const char *package) {
  if (!ctx || !ctx->environment)
    return false;
  return th_ctx_set_scope(ctx, module, package);
}

static bool visible_to(const th_ctx *ctx, th_env_visibility_t visibility,
                       const char *module, const char *package) {
  if (visibility == TH_ENV_PUBLIC)
    return true;
  if (visibility == TH_ENV_PACKAGE)
    return ctx->package && !strcmp(ctx->package, package);
  return ctx->module && !strcmp(ctx->module, module);
}

bool th_env_type_visible(const th_ctx *ctx, const char *name) {
  if (!ctx || !ctx->environment || !name)
    return false;
  const th_env *env = ctx->environment;
  for (size_t i = 0; i < env->type_origins.len; i++) {
    const th_env_type_origin_t *origin = &env->type_origins.data[i];
    if (!strcmp(origin->name, name))
      return visible_to(ctx, origin->visibility, origin->module,
                        origin->package);
  }
  return false;
}

th_type *th_env_type_shape(const th_ctx *ctx, const char *name,
                           th_env_type_shape_kind_t *shape_kind) {
  if (shape_kind) *shape_kind = TH_ENV_TYPE_SHAPE_NONE;
  if (!ctx || !ctx->environment || !name) return nullptr;
  const th_env *env = ctx->environment;
  for (size_t i = 0; i < env->type_origins.len; i++) {
    const th_env_type_origin_t *origin = &env->type_origins.data[i];
    if (strcmp(origin->name, name) ||
        !visible_to(ctx, origin->visibility, origin->module,
                    origin->package)) continue;
    if (shape_kind) *shape_kind = origin->shape_kind;
    return origin->shape;
  }
  return nullptr;
}

static th_env_type_origin_t *receiver_origin(const th_env *env,
                                             th_type *receiver) {
  receiver = th_resolve(receiver);
  const char *name = nullptr;
  size_t nlen = 0;
  if (!th_named_info(receiver, &name, &nlen, nullptr, nullptr))
    return nullptr;
  for (size_t i = 0; i < env->type_origins.len; i++) {
    th_env_type_origin_t *origin = &env->type_origins.data[i];
    if (strlen(origin->name) == nlen && !memcmp(origin->name, name, nlen))
      return origin;
  }
  return nullptr;
}

static int function_origin_rank(const th_env_type_origin_t *origin,
                                const th_env_function_t *function) {
  if (origin && !strcmp(origin->module, function->module))
    return 2;
  if (origin && !strcmp(origin->package, function->package))
    return 1;
  return 0;
}

static bool function_matches(th_ctx *ctx, th_type *receiver,
                             const th_env_trait_t *trait,
                             const th_env_function_t *function,
                             const char *method_name) {
  th_type *trait_contracts = fresh_pattern(ctx, trait->contracts);
  th_type *function_contract = fresh_pattern(ctx, function->contract);
  th_type *required_contract =
      th_record_lookup(trait_contracts, method_name);
  required_contract = required_contract
      ? fresh_pattern(ctx, required_contract) : nullptr;
  th_type *provided = nullptr, *provided_error = nullptr;
  th_type *required = nullptr, *required_error = nullptr;
  signature_channels_raw(ctx, required_contract, &required,
                         &required_error);
  signature_channels_raw(ctx, function_contract, &provided,
                         &provided_error);
  th_type *trait_receiver = first_parameter(required);
  th_type *function_receiver = first_parameter(provided);
  th_type *need[] = {trait_receiver, function_receiver, required_contract};
  th_type *have[] = {receiver, receiver, function_contract};
  bool matches = required && provided && trait_receiver && function_receiver &&
                 required_error && provided_error &&
                 th_unify(ctx, th_tuple(ctx, need, 3),
                          th_tuple(ctx, have, 3)) != nullptr;
  return matches;
}

static bool function_matches_with_promotion(
    th_ctx *ctx, th_type *receiver, const th_env_trait_t *trait,
    const th_env_function_t *function, const char *method_name) {
  if (function_matches(ctx, receiver, trait, function, method_name))
    return true;
  th_type *signature = nullptr;
  th_env_signature_channels(ctx, fresh_pattern(ctx, function->contract),
                            &signature, nullptr);
  th_type *promoted_receiver = first_parameter(signature);
  bool promoted = promoted_receiver &&
                  th_type_promotes_to(ctx, receiver, promoted_receiver) &&
                  function_matches(ctx, promoted_receiver, trait, function,
                                   method_name);
  return promoted;
}

static bool function_can_match(th_ctx *ctx, const th_env_trait_t *trait,
                               const th_env_function_t *function,
                               const char *method_name) {
  th_type *trait_contracts = fresh_pattern(ctx, trait->contracts);
  th_type *function_contract = fresh_pattern(ctx, function->contract);
  th_type *required_contract =
      th_record_lookup(trait_contracts, method_name);
  required_contract = required_contract
      ? fresh_pattern(ctx, required_contract) : nullptr;
  th_type *required = nullptr, *required_error = nullptr;
  th_type *provided = nullptr, *provided_error = nullptr;
  signature_channels_raw(ctx, required_contract, &required,
                         &required_error);
  signature_channels_raw(ctx, function_contract, &provided,
                         &provided_error);
  bool matches = required && provided && first_parameter(required) &&
                 first_parameter(provided) && required_error && provided_error;
  if (matches) {
    matches = th_unify(ctx, required_contract, function_contract) != nullptr;
  }
  return matches;
}

static const th_env_trait_t *visible_trait(const th_ctx *ctx,
                                           const char *name) {
  if (!ctx || !ctx->environment || !name)
    return nullptr;
  th_env_trait_t *trait = trait_named(ctx->environment, name);
  return trait && visible_to(ctx, trait->visibility, trait->module,
                             trait->package)
             ? trait
             : nullptr;
}

typedef struct {
  const th_env *environment;
  core_datalog_symbol_t trait;
  core_datalog_symbol_t method;
  th_ctx *context;
  core_list_t(th_env_function_ptr_t) * functions;
} candidate_query_t;

static bool collect_candidate(const core_datalog_symbol_t *tuple, int32_t arity,
                              void *raw) {
  candidate_query_t *query = raw;
  if (arity != 3 || tuple[0] != query->trait || tuple[1] != query->method)
    return true;
  core_datalog_int_result_t index =
      core_datalog_symbol_integer(query->environment->logic, tuple[2]);
  if (!index.is_ok || index.result.value < 0 ||
      (uint64_t)index.result.value >= query->environment->functions.len)
    return true;
  const th_env_function_t *function =
      &query->environment->functions.data[index.result.value];
  if (visible_to(query->context, function->visibility, function->module,
                 function->package))
    core_list_push(*query->functions, function);
  return true;
}

static core_list_t(th_env_function_ptr_t)
    candidate_functions(th_ctx *ctx, const th_env_trait_t *trait,
                        const char *method_name) {
  core_list_t(th_env_function_ptr_t) functions =
      core_list_new(th_env_function_ptr_t);
  if (!ctx || !ctx->environment || !ctx->environment->frozen || !trait ||
      !method_name)
    return functions;
  size_t trait_index = (size_t)(trait - ctx->environment->traits.data);
  core_datalog_symbol_t method_symbol = CORE_DATALOG_SYMBOL_INVALID;
  for (size_t i = 0; i < ctx->environment->method_symbols.len; i++) {
    const th_env_method_symbol_t *entry =
        &ctx->environment->method_symbols.data[i];
    if (entry->trait_index == trait_index &&
        !strcmp(entry->method_name, method_name)) {
      method_symbol = entry->logic_symbol;
      break;
    }
  }
  if (method_symbol == CORE_DATALOG_SYMBOL_INVALID)
    return functions;
  candidate_query_t query = {
      .environment = ctx->environment,
      .trait = trait->logic_symbol,
      .method = method_symbol,
      .context = ctx,
      .functions = &functions,
  };
  core_datalog_query(ctx->environment->logic,
                     ctx->environment->candidate_relation, collect_candidate,
                     &query);
  return functions;
}

typedef struct {
  const th_env *environment;
  th_ctx *context;
  core_datalog_symbol_t trait;
  th_type *receiver;
  bool matched;
} intrinsic_query_t;

static bool same_intrinsic_head(const th_type *pattern,
                                const th_type *receiver) {
  const char *pattern_name = nullptr;
  const char *receiver_name = nullptr;
  size_t pattern_len = 0, receiver_len = 0;
  size_t pattern_arity = 0, receiver_arity = 0;
  bool pattern_variant = false, receiver_variant = false;
  bool pattern_named = th_named_info(pattern, &pattern_name, &pattern_len,
                                     &pattern_variant, &pattern_arity);
  bool receiver_named = th_named_info(receiver, &receiver_name,
                                       &receiver_len, &receiver_variant,
                                       &receiver_arity);
  if (th_kind_of(pattern) != th_kind_of(receiver))
    return false;
  if (!pattern_named || !receiver_named) return true;
  return pattern_len == receiver_len && pattern_arity == receiver_arity &&
         pattern_variant == receiver_variant &&
         !memcmp(pattern_name, receiver_name, pattern_len);
}

static bool collect_intrinsic_type(const core_datalog_symbol_t *tuple,
                                   int32_t arity, void *raw) {
  intrinsic_query_t *query = raw;
  if (query->matched || arity != 2 || tuple[0] != query->trait)
    return true;
  core_datalog_int_result_t index =
      core_datalog_symbol_integer(query->environment->logic, tuple[1]);
  if (!index.is_ok || index.result.value < 0 ||
      (uint64_t)index.result.value >= query->environment->intrinsic_types.len)
    return true;
  const th_env_intrinsic_type_t *fact =
      &query->environment->intrinsic_types.data[index.result.value];
  if (!same_intrinsic_head(fact->receiver_pattern, query->receiver))
    return true;
  th_type *pattern = fresh_pattern(query->context,
                                   fact->receiver_pattern);
  query->matched = pattern && th_unify_probe(query->context, pattern,
                                             query->receiver) != nullptr;
  return true;
}

static bool intrinsic_type_matches(th_ctx *ctx, const th_env_trait_t *trait,
                                   th_type *receiver) {
  if (!ctx || !ctx->environment || !trait || !receiver)
    return false;
  intrinsic_query_t query = {
      .environment = ctx->environment,
      .context = ctx,
      .trait = trait->logic_symbol,
      .receiver = receiver,
  };
  core_datalog_query(ctx->environment->logic,
                     ctx->environment->intrinsic_type_relation,
                     collect_intrinsic_type, &query);
  return query.matched;
}

bool th_env_implements(th_ctx *ctx, th_type *receiver, const char *trait_name) {
  if (!ctx || !ctx->environment || !ctx->environment->frozen || !receiver ||
      !trait_name || th_kind_of(th_resolve(receiver)) == TH_VAR)
    return false;
  const th_env_trait_t *trait = visible_trait(ctx, trait_name);
  if (!trait)
    return false;
  th_type *methods = fresh_pattern(ctx, trait->contracts);
  if (!th_record_len(methods))
    return intrinsic_type_matches(ctx, trait, receiver);
  for (size_t m = 0; m < th_record_len(methods); m++) {
    const th_member *required = th_record_member(methods, m);
    core_list_t(th_env_function_ptr_t) candidates =
        candidate_functions(ctx, trait, required->name);
    bool found = false;
    for (size_t i = 0; required && i < candidates.len; i++) {
      const th_env_function_t *function = candidates.data[i];
      if (function_matches_with_promotion(ctx, receiver, trait, function,
                                          required->name)) {
        found = true;
        break;
      }
    }
    core_list_free(candidates);
    if (!found)
      return false;
  }
  return true;
}

th_env_method_status_t th_env_method_resolve(th_ctx *ctx, th_type *receiver,
                                             const char *trait_name,
                                             const char *method_name,
                                             th_env_method_t *out) {
  if (!ctx || !ctx->environment || !receiver || !trait_name || !method_name ||
      !th_env_implements(ctx, receiver, trait_name))
    return TH_ENV_METHOD_NOT_FOUND;
  const th_env_trait_t *trait = visible_trait(ctx, trait_name);
  th_type *trait_methods = trait ? fresh_pattern(ctx, trait->contracts) : nullptr;
  if (!trait_methods || !th_record_lookup(trait_methods, method_name))
    return TH_ENV_METHOD_NOT_FOUND;
  th_env_type_origin_t *origin = receiver_origin(ctx->environment, receiver);
  typedef struct {
    const th_env_function_t *function;
    th_type *receiver_pattern;
  } match_t;
  core_list_decl(match_t);
  core_list_t(match_t) matches = core_list_new(match_t);
  core_list_t(th_env_function_ptr_t) candidates =
      candidate_functions(ctx, trait, method_name);
  for (size_t i = 0; i < candidates.len; i++) {
    const th_env_function_t *function = candidates.data[i];
    if (!function_matches_with_promotion(ctx, receiver, trait, function,
                                         method_name))
      continue;
    th_type *signature = nullptr;
    th_env_signature_channels(ctx, fresh_pattern(ctx, function->contract),
                              &signature, nullptr);
    th_type *pattern = first_parameter(signature);
    if (pattern)
      core_list_push(matches, ((match_t){function, pattern}));
  }
  core_list_free(candidates);
  bool *maximal = matches.len ? core_alloc_array(bool, matches.len) : nullptr;
  for (size_t i = 0; i < matches.len; i++) {
    maximal[i] = true;
    for (size_t j = 0; j < matches.len; j++) {
      if (i == j)
        continue;
      bool i_sub_j =
          receiver_subsumes(matches.data[j].receiver_pattern,
                            matches.data[i].receiver_pattern) ||
          th_type_promotes_to(ctx, matches.data[i].receiver_pattern,
                              matches.data[j].receiver_pattern);
      bool j_sub_i =
          receiver_subsumes(matches.data[i].receiver_pattern,
                            matches.data[j].receiver_pattern) ||
          th_type_promotes_to(ctx, matches.data[j].receiver_pattern,
                              matches.data[i].receiver_pattern);
      if (j_sub_i && !i_sub_j) {
        maximal[i] = false;
        break;
      }
    }
  }
  const th_env_function_t *best = nullptr;
  int best_origin_rank = -1;
  size_t best_origin_count = 0;
  bool ambiguous = false;
  for (size_t i = 0; i < matches.len; i++) {
    if (!maximal[i])
      continue;
    if (!best) {
      best = matches.data[i].function;
      best_origin_rank = function_origin_rank(origin, best);
      best_origin_count = 1;
      continue;
    }
    bool equivalent = true;
    for (size_t j = 0; equivalent && j < matches.len; j++) {
      if (!maximal[j] || i == j)
        continue;
      equivalent = receiver_subsumes(matches.data[i].receiver_pattern,
                                     matches.data[j].receiver_pattern) &&
                   receiver_subsumes(matches.data[j].receiver_pattern,
                                     matches.data[i].receiver_pattern);
    }
    if (!equivalent) {
      ambiguous = true;
      break;
    }
    int origin_rank = function_origin_rank(origin,
                                           matches.data[i].function);
    if (origin_rank > best_origin_rank) {
      best = matches.data[i].function;
      best_origin_rank = origin_rank;
      best_origin_count = 1;
    }
    else if (origin_rank == best_origin_rank) {
      if (strcmp(matches.data[i].function->module, best->module))
        best_origin_count++;
    }
  }
  core_free(maximal);
  core_list_free(matches);
  if (ambiguous || best_origin_count > 1)
    return TH_ENV_METHOD_AMBIGUOUS;
  if (!best)
    return TH_ENV_METHOD_NOT_FOUND;
  th_type *signature = nullptr;
  th_type *error_type = nullptr;
  if (!th_env_signature_channels(ctx, best->contract, &signature,
                                 &error_type))
    return TH_ENV_METHOD_NOT_FOUND;
  if (out)
    *out = (th_env_method_t){
        .qualified_name = best->qualified_name,
        .root_name = best->root_name,
        .module = best->module,
        .package = best->package,
        .signature = signature,
        .error_type = error_type,
        .contract = best->contract,
    };
  return TH_ENV_METHOD_FOUND;
}

bool th_env_method_lookup(th_ctx *ctx, th_type *receiver,
                          const char *trait_name, const char *method_name,
                          th_env_method_t *out) {
  return th_env_method_resolve(ctx, receiver, trait_name, method_name, out) ==
         TH_ENV_METHOD_FOUND;
}

th_env_method_status_t th_env_method_resolve_by_name(
    th_ctx *ctx, th_type *receiver, const char *method_name,
    th_env_method_t *out, const char **trait_name) {
  if (out) *out = (th_env_method_t){0};
  if (trait_name) *trait_name = nullptr;
  if (!ctx || !ctx->environment || !ctx->environment->frozen || !receiver ||
      !method_name)
    return TH_ENV_METHOD_NOT_FOUND;

  th_env_method_t selected = {0};
  const char *selected_trait = nullptr;
  for (size_t i = 0; i < ctx->environment->traits.len; i++) {
    const th_env_trait_t *trait = &ctx->environment->traits.data[i];
    if (!visible_to(ctx, trait->visibility, trait->module, trait->package) ||
        !th_record_lookup(trait->scheme, method_name))
      continue;
    th_env_method_t candidate = {0};
    th_env_method_status_t status = th_env_method_resolve(
        ctx, receiver, trait->name, method_name, &candidate);
    if (status == TH_ENV_METHOD_AMBIGUOUS)
      return status;
    if (status != TH_ENV_METHOD_FOUND)
      continue;
    if (selected.qualified_name &&
        strcmp(selected.qualified_name, candidate.qualified_name))
      return TH_ENV_METHOD_AMBIGUOUS;
    if (!selected.qualified_name) {
      selected = candidate;
      selected_trait = trait->name;
    }
  }
  if (!selected.qualified_name)
    return TH_ENV_METHOD_NOT_FOUND;
  if (out) *out = selected;
  if (trait_name) *trait_name = selected_trait;
  return TH_ENV_METHOD_FOUND;
}

size_t th_env_method_candidates(th_ctx *ctx, const char *trait_name,
                                const char *method_name, th_env_method_t *out,
                                size_t max_methods) {
  if (!ctx || !ctx->environment || !trait_name || !method_name)
    return 0;
  const th_env_trait_t *trait = visible_trait(ctx, trait_name);
  th_type *methods = trait ? fresh_pattern(ctx, trait->contracts) : nullptr;
  if (!methods || !th_record_lookup(methods, method_name))
    return 0;
  core_list_t(th_env_function_ptr_t) candidates =
      candidate_functions(ctx, trait, method_name);
  size_t count = 0;
  for (size_t i = 0; i < candidates.len; i++) {
    const th_env_function_t *function = candidates.data[i];
    if (out && count < max_methods) {
      th_type *signature = nullptr, *error_type = nullptr;
      if (!th_env_signature_channels(ctx, function->contract, &signature,
                                     &error_type))
        continue;
      out[count] = (th_env_method_t){
          .qualified_name = function->qualified_name,
          .root_name = function->root_name,
          .module = function->module,
          .package = function->package,
          .signature = signature,
          .error_type = error_type,
          .contract = function->contract,
      };
    }
    count++;
  }
  core_list_free(candidates);
  return count;
}

th_env_method_status_t th_env_method_plan_dispatch(
    th_ctx *ctx, const char *trait_name, const char *method_name,
    th_type *const *receiver_types, size_t receiver_count,
    th_env_dispatch_entry_t *entries, size_t capacity,
    size_t *required_count, bool *direct_call) {
  if (required_count) *required_count = 0;
  if (direct_call) *direct_call = false;
  if (!ctx || !ctx->environment || !ctx->environment->frozen ||
      !trait_name || !method_name || !receiver_types || !receiver_count)
    return TH_ENV_METHOD_NOT_FOUND;

  th_env_dispatch_entry_t *plan =
      core_alloc_array(th_env_dispatch_entry_t, receiver_count);
  bool single_target = true;
  for (size_t i = 0; i < receiver_count; i++) {
    if (!receiver_types[i]) {
      core_free(plan);
      return TH_ENV_METHOD_NOT_FOUND;
    }
    th_env_method_status_t status = th_env_method_resolve(
        ctx, receiver_types[i], trait_name, method_name, &plan[i].method);
    if (status != TH_ENV_METHOD_FOUND) {
      core_free(plan);
      return status;
    }
    plan[i].receiver_type = receiver_types[i];
    if (i && strcmp(plan[0].method.qualified_name,
                    plan[i].method.qualified_name))
      single_target = false;
  }

  if (required_count) *required_count = receiver_count;
  if (direct_call) *direct_call = single_target;
  if (entries) {
    size_t copied = capacity < receiver_count ? capacity : receiver_count;
    memcpy(entries, plan, copied * sizeof(*entries));
  }
  core_free(plan);
  return TH_ENV_METHOD_FOUND;
}

th_env_method_status_t th_env_trait_vtable_plan(
    th_ctx *ctx, const char *trait_name, th_type *concrete_type,
    th_env_vtable_slot_t *slots, size_t capacity, size_t *required_count,
    th_env_vtable_plan_t *out_plan) {
  if (required_count) *required_count = 0;
  if (out_plan) *out_plan = (th_env_vtable_plan_t){0};
  if (!ctx || !ctx->environment || !ctx->environment->frozen ||
      !trait_name || !concrete_type)
    return TH_ENV_METHOD_NOT_FOUND;
  const th_env_trait_t *trait = visible_trait(ctx, trait_name);
  if (!trait)
    return TH_ENV_METHOD_NOT_FOUND;
  size_t count = th_record_len(trait->contracts);
  th_env_vtable_slot_t *plan =
      core_alloc_array(th_env_vtable_slot_t, count);
  for (size_t i = 0; i < count; i++) {
    const th_member *requirement = th_record_member(trait->contracts, i);
    th_env_method_status_t status = th_env_method_resolve(
        ctx, concrete_type, trait_name, requirement->name,
        &plan[i].implementation);
    if (status != TH_ENV_METHOD_FOUND) {
      core_free(plan);
      return status;
    }
    plan[i].method_name = requirement->name;
  }
  if (required_count) *required_count = count;
  if (out_plan) {
    size_t copied = slots ? (capacity < count ? capacity : count) : 0;
    *out_plan = (th_env_vtable_plan_t){
        .trait_name = trait->name,
        .concrete_type = concrete_type,
        .concrete_type_hash = th_hash(ctx, concrete_type),
        .slots = slots,
        .slot_count = copied,
        .required_slot_count = count,
    };
  }
  if (slots) {
    size_t copied = capacity < count ? capacity : count;
    memcpy(slots, plan, copied * sizeof(*slots));
  }
  core_free(plan);
  return TH_ENV_METHOD_FOUND;
}

size_t th_env_method_requirements(
    th_ctx *ctx, const char *method_name,
    th_env_method_requirement_t *requirements, size_t max_requirements) {
  if (!ctx || !ctx->environment || !ctx->environment->frozen ||
      !method_name)
    return 0;
  const th_env *env = ctx->environment;
  size_t count = 0;
  for (size_t i = 0; i < env->traits.len; i++) {
    const th_env_trait_t *trait = &env->traits.data[i];
    if (!visible_to(ctx, trait->visibility, trait->module, trait->package) ||
        !th_record_lookup(trait->contracts, method_name))
      continue;
    if (requirements && count < max_requirements) {
      th_type *methods = fresh_pattern(ctx, trait->contracts);
      th_type *contract = th_record_lookup(methods, method_name);
      th_type *signature = nullptr, *error_type = nullptr;
      if (th_env_signature_channels(ctx, contract, &signature, &error_type))
        requirements[count] = (th_env_method_requirement_t){
            .trait_name = trait->name,
            .signature = signature,
            .error_type = error_type,
            .contract = contract,
        };
    }
    count++;
  }
  return count;
}
