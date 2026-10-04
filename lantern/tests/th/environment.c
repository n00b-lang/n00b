#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "th/environment.h"

#define TEST_ENV_ROOT_TYPE UINT64_C(0x7468656e762e726f)
#define TEST_ENV_ARCHIVE_ABI UINT64_C(0x7468656e76617204)

typedef struct {
  uint64_t id;
  uint64_t type_id;
  uint32_t schema_version;
  uint32_t reserved;
  size_t payload_size;
  const uint8_t *payload;
} test_archive_record_t;

typedef struct {
  uint64_t magic;
  uint32_t version;
  uint32_t reserved;
  uint64_t root_type_id;
  uint64_t root_object_id;
  size_t record_count;
  const test_archive_record_t *records;
} test_archive_image_t;

static uint64_t read_le_u64(const uint8_t *bytes) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; i++)
    value |= (uint64_t)bytes[i] << (i * 8);
  return value;
}

static bool skip_archive_string(const uint8_t *payload, size_t size,
                                size_t *offset) {
  if (*offset > size || size - *offset < 8) return false;
  uint64_t length = read_le_u64(payload + *offset);
  *offset += 8;
  if (length == UINT64_MAX) return true;
  if (length > size - *offset) return false;
  *offset += (size_t)length;
  return true;
}

static bool skip_archive_strings(const uint8_t *payload, size_t size,
                                 size_t *offset, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (!skip_archive_string(payload, size, offset)) return false;
  return true;
}

static bool mutate_environment_visibility(const char *path, bool trait,
                                          uint32_t value) {
  core_persist_arena_t *arena = nullptr;
  core_persist_arena_error_t error;
  if (!core_persist_arena_open(path,
                               core_persist_arena_native_abi_tag(
                                   TEST_ENV_ARCHIVE_ABI),
                               true, &arena,
                               &error)) {
    return false;
  }
  test_archive_image_t *image = core_persist_arena_root(arena);
  bool valid = core_persist_arena_contains(arena, image, sizeof(*image));
  test_archive_record_t *root_record = nullptr;
  if (valid && image->record_count <= SIZE_MAX / sizeof(*image->records) &&
      core_persist_arena_contains(
          arena, image->records,
          image->record_count * sizeof(*image->records))) {
    for (size_t i = 0; i < image->record_count; i++)
      if (image->records[i].type_id == TEST_ENV_ROOT_TYPE) {
        root_record = (test_archive_record_t *)&image->records[i];
        break;
      }
  }
  valid = valid && root_record && root_record->payload_size &&
          core_persist_arena_contains(arena, root_record->payload,
                                      root_record->payload_size);
  if (valid) {
    const uint8_t *payload = root_record->payload;
    size_t size = root_record->payload_size;
    size_t offset = 0;
    if (size < 8) valid = false;
    size_t origin_count = valid ? (size_t)read_le_u64(payload) : 0;
    offset = 8;
    for (size_t i = 0; valid && i < origin_count; i++) {
      valid = skip_archive_strings(payload, size, &offset, 3) &&
              offset <= size && size - offset >= 24;
      if (valid) offset += 24;
    }
    size_t trait_count = 0;
    if (valid && offset <= size && size - offset >= 8) {
      trait_count = (size_t)read_le_u64(payload + offset);
      offset += 8;
    } else valid = false;
    size_t target = 0;
    if (valid && trait && trait_count) {
      valid = skip_archive_strings(payload, size, &offset, 3) &&
              offset <= size && size - offset >= 24;
      target = offset;
    } else if (valid) {
      for (size_t i = 0; valid && i < trait_count; i++) {
        valid = skip_archive_strings(payload, size, &offset, 3) &&
                offset <= size && size - offset >= 52;
        if (valid) offset += 52;
      }
      if (valid && offset <= size && size - offset >= 8) {
        size_t function_count = (size_t)read_le_u64(payload + offset);
        offset += 8;
        valid = function_count &&
                skip_archive_strings(payload, size, &offset, 4) &&
                offset <= size && size - offset >= 4;
        target = offset;
      } else valid = false;
    } else {
      valid = false;
    }
    if (valid) {
      uint8_t *mutable_payload = (uint8_t *)root_record->payload;
      for (unsigned i = 0; i < 4; i++)
        mutable_payload[target + i] = (uint8_t)(value >> (i * 8));
      valid = core_persist_arena_sync(arena, &error);
    }
  }
  bool closed = core_persist_arena_close(arena, &error);
  return valid && closed;
}

static bool mutate_environment_metadata_owner(const char *path,
                                              uint32_t value) {
  core_persist_arena_t *arena = nullptr;
  core_persist_arena_error_t error;
  if (!core_persist_arena_open(path,
                               core_persist_arena_native_abi_tag(
                                   TEST_ENV_ARCHIVE_ABI),
                               true, &arena,
                               &error))
    return false;
  test_archive_image_t *image = core_persist_arena_root(arena);
  bool valid = core_persist_arena_contains(arena, image, sizeof(*image)) &&
      image->record_count <= SIZE_MAX / sizeof(*image->records) &&
      core_persist_arena_contains(
          arena, image->records,
          image->record_count * sizeof(*image->records));
  test_archive_record_t *root_record = nullptr;
  for (size_t i = 0; valid && i < image->record_count; i++)
    if (image->records[i].type_id == TEST_ENV_ROOT_TYPE) {
      root_record = (test_archive_record_t *)&image->records[i];
      break;
    }
  valid = valid && root_record && root_record->payload_size &&
          core_persist_arena_contains(arena, root_record->payload,
                                      root_record->payload_size);
  size_t offset = 0;
  const uint8_t *payload = valid ? root_record->payload : nullptr;
  size_t size = valid ? root_record->payload_size : 0;
  if (valid && size >= 8) {
    size_t count = (size_t)read_le_u64(payload + offset);
    offset += 8;
    for (size_t i = 0; valid && i < count; i++) {
      valid = skip_archive_strings(payload, size, &offset, 3) &&
              offset <= size && size - offset >= 24;
      if (valid) offset += 24;
    }
  } else valid = false;
  for (unsigned group = 0; valid && group < 2; group++) {
    if (offset > size || size - offset < 8) {
      valid = false;
      break;
    }
    size_t count = (size_t)read_le_u64(payload + offset);
    offset += 8;
    for (size_t i = 0; valid && i < count; i++) {
      size_t strings = group == 0 ? 3 : 4;
      valid = skip_archive_strings(payload, size, &offset, strings) &&
              offset <= size && size - offset >= 52;
      if (valid) offset += 52;
    }
  }
  if (valid && root_record->schema_version >= 8) {
    valid = offset <= size && size - offset >= 8;
    size_t count = valid ? (size_t)read_le_u64(payload + offset) : 0;
    if (valid) offset += 8;
    for (size_t i = 0; valid && i < count; i++) {
      valid = skip_archive_strings(payload, size, &offset, 4) &&
              offset <= size && size - offset >= 20;
      if (valid) offset += 20;
    }
  }
  size_t metadata_count = 0;
  if (valid && offset <= size && size - offset >= 8) {
    metadata_count = (size_t)read_le_u64(payload + offset);
    offset += 8;
  } else valid = false;
  valid = valid && metadata_count && offset <= size && size - offset >= 4;
  if (valid) {
    uint8_t *mutable_payload = (uint8_t *)root_record->payload;
    for (unsigned i = 0; i < 4; i++)
      mutable_payload[offset + i] = (uint8_t)(value >> (i * 8));
    valid = core_persist_arena_sync(arena, &error);
  }
  bool closed = core_persist_arena_close(arena, &error);
  return valid && closed;
}

static th_type *fn1(th_ctx *ctx, th_type *receiver, th_type *result) {
  th_param param = {.mode = TH_IN, .type = receiver};
  return th_func(ctx, result, &param, 1, nullptr, nullptr, 0);
}

static th_type *fn0(th_ctx *ctx, th_type *result) {
  return th_func(ctx, result, nullptr, 0, nullptr, nullptr, 0);
}

static th_type *fn2(th_ctx *ctx, th_type *receiver, th_type *arg,
                    th_type *result) {
  th_param params[] = {
      {.mode = TH_IN, .type = receiver},
      {.mode = TH_IN, .type = arg},
  };
  return th_func(ctx, result, params, 2, nullptr, nullptr, 0);
}

static th_type *method_record(th_ctx *ctx, const char *name,
                              th_type *signature) {
  th_member method = {.name = name, .type = signature};
  return th_record(ctx, &method, 1);
}

static bool register_function(th_env *env, const char *qualified_name,
                              const char *root, const char *module,
                              const char *package,
                              th_env_visibility_t visibility,
                              th_type *signature) {
  return th_env_register_function(env, qualified_name, root, module, package,
                                  visibility, signature);
}

typedef struct {
  const th_env *environment;
  bool ok;
  bool composed_ok;
} environment_query_run_t;

static void *query_frozen_environment(void *raw) {
  environment_query_run_t *run = raw;
  th_ctx *context = th_ctx_new();
  run->ok =
      th_ctx_set_environment(context, run->environment, "consumer", "base");
  for (int i = 0; run->ok && i < 100; i++) {
    th_type *integer = th_name(context, "int");
    th_env_method_t method = {0};
    run->ok = th_env_implements(context, integer, "Readable") &&
              th_env_method_candidates(context, "Readable", "read", nullptr,
                                       0) == 3 &&
              th_env_method_resolve(context, integer, "Readable", "read",
                                    &method) == TH_ENV_METHOD_FOUND &&
              !strcmp(method.qualified_name, "base.ints.read_first");
  }
  th_arg integer_arg = {.kind = TH_ARG_TYPE,
                        .type = th_name(context, "int")};
  th_type *box_integer = th_apply(context, "Box", &integer_arg, 1);
  th_env_dispatch_entry_t entry = {0};
  bool direct_call = false;
  run->ok = run->ok && th_env_method_plan_dispatch(
      context, "BoxReadable", "get", &box_integer, 1, &entry, 1, nullptr,
      &direct_call) == TH_ENV_METHOD_FOUND && direct_call &&
      !strcmp(entry.method.qualified_name, "base.box.int_get");

  const th_env *sources[] = {run->environment};
  th_env *composed = th_env_compose(sources, 1);
  th_ctx *composed_context = th_ctx_new();
  run->composed_ok = composed && th_env_freeze(composed) &&
      th_ctx_set_environment(composed_context, composed, "consumer", "base");
  if (run->composed_ok) {
    th_type *composed_integer = th_name(composed_context, "int");
    th_env_method_t method = {0};
    run->composed_ok = th_env_method_resolve(
        composed_context, composed_integer, "Readable", "read", &method) ==
            TH_ENV_METHOD_FOUND &&
        !strcmp(method.qualified_name, "base.ints.read_first");
  }
  th_ctx_free(composed_context);
  th_env_free(composed);
  th_ctx_free(context);
  return nullptr;
}

int main(void) {
  th_env *env = th_env_new();
  th_ctx *types = th_ctx_new();
  th_type *integer = th_name(types, "int");
  th_type *string = th_name(types, "string");

  check("register type origin",
        th_env_register_type(env, "int", "ints", "base", TH_ENV_PUBLIC));
  check("type origin visibility is part of its stable declaration",
        !th_env_register_type(env, "int", "ints", "base", TH_ENV_PRIVATE) &&
            th_env_register_type(env, "Secret", "secrets", "base",
                                 TH_ENV_PRIVATE) &&
            th_env_register_type(env, "PackageType", "internal", "base",
                                 TH_ENV_PACKAGE));
  th_ctx *utility_types = th_ctx_new();
  check("ordinary function signature is borrowed from its module context",
        register_function(env, "base.utility", "utility", "utility", "base",
                          TH_ENV_PUBLIC,
                          fn0(utility_types, th_name(utility_types, "int"))));
  check("duplicate qualified function identities are rejected",
        !register_function(
            env, "base.utility", "utility", "utility", "base", TH_ENV_PUBLIC,
            fn0(utility_types, th_name(utility_types, "string"))));

  th_type *read_receiver = th_var(types);
  check("register trait with unbound receiver parameter",
        th_env_register_trait(
            env, "Readable", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "read", fn1(types, read_receiver, integer))));
  check("reject trait with concrete receiver parameter",
        !th_env_register_trait(
            env, "Bad", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "bad", fn1(types, integer, integer))));

  th_type *read_int = fn1(types, integer, integer);
  check("register first package method candidate",
        register_function(env, "base.ints.read_first", "read", "ints", "base",
                          TH_ENV_PACKAGE, read_int));
  check("register same-rank method candidate in import order",
        register_function(env, "base.ints.read_second", "read", "ints", "base",
                          TH_ENV_PACKAGE, read_int));
  check("register package fallback candidate",
        register_function(env, "base.other.read", "read", "other", "base",
                          TH_ENV_PACKAGE, read_int));

  th_type *paired_receiver = th_var(types);
  th_member paired_requirements[] = {
      {.name = "first",
       .type = fn2(types, paired_receiver, paired_receiver, integer)},
      {.name = "second",
       .type = fn2(types, paired_receiver, paired_receiver, integer)},
  };
  check("register trait with shared receiver variable",
        th_env_register_trait(env, "Paired", "traits", "base", TH_ENV_PUBLIC,
                              th_record(types, paired_requirements, 2)));
  check("register first matching function for paired trait",
        register_function(env, "base.impl.first", "first", "impl", "base",
                          TH_ENV_PUBLIC,
                          fn2(types, integer, integer, integer)));
  check("register mismatching second function for paired trait",
        register_function(env, "base.impl.second", "second", "impl", "base",
                          TH_ENV_PUBLIC, fn2(types, integer, string, integer)));

  th_type *trait_element = th_var(types);
  check("register generic Box trait",
        th_env_register_trait(
            env, "BoxReadable", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "get",
                          fn1(types, trait_element, trait_element))));

  th_type *generic_element = th_var(types);
  th_arg generic_box_arg = {.kind = TH_ARG_TYPE, .type = generic_element};
  th_type *generic_box = th_apply(types, "Box", &generic_box_arg, 1);
  check("register generic Box function candidate",
        register_function(env, "base.box.generic_get", "get", "box-generic",
                          "base", TH_ENV_PACKAGE,
                          fn1(types, generic_box, generic_box)));

  th_arg concrete_box_arg = {.kind = TH_ARG_TYPE, .type = integer};
  th_type *concrete_box = th_apply(types, "Box", &concrete_box_arg, 1);
  check("register concrete Box function candidate",
        register_function(env, "base.box.int_get", "get", "box-int", "base",
                          TH_ENV_PACKAGE,
                          fn1(types, concrete_box, concrete_box)));

  th_type *echo_receiver = th_var(types);
  check("register a self-returning method trait",
        th_env_register_trait(
            env, "SelfEcho", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "echo",
                          fn1(types, echo_receiver, echo_receiver))));
  check("register a concrete self-returning method",
        register_function(env, "base.box.self_echo", "echo", "box-int",
                          "base", TH_ENV_PACKAGE,
                          fn1(types, concrete_box, concrete_box)));

  th_type *returns_argument_receiver = th_var(types);
  th_type *returns_argument_value = th_var(types);
  check("register trait with an independent generic argument and result",
        th_env_register_trait(
            env, "ReturnsArgument", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "echo",
                fn2(types, returns_argument_receiver, returns_argument_value,
                    returns_argument_value))));
  th_type *echo_value = th_var(types);
  check("register generic method implementation",
        register_function(env, "base.ints.echo", "echo", "ints", "base",
                          TH_ENV_PACKAGE,
                          fn2(types, integer, echo_value, echo_value)));

  th_type *pair_trait_receiver = th_var(types);
  check("register trait used to test overlapping receiver patterns",
        th_env_register_trait(
            env, "PairReadable", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "get",
                          fn1(types, pair_trait_receiver, integer))));
  th_type *same_arg = th_var(types);
  th_arg same_pair_args[] = {
      {.kind = TH_ARG_TYPE, .type = same_arg},
      {.kind = TH_ARG_TYPE, .type = same_arg},
  };
  th_arg first_int_pair_args[] = {
      {.kind = TH_ARG_TYPE, .type = integer},
      {.kind = TH_ARG_TYPE, .type = th_var(types)},
  };
  check("register repeated-variable pair method",
        register_function(
            env, "base.pairs.same_get", "get", "pairs", "base", TH_ENV_PUBLIC,
            fn1(types, th_apply(types, "Pair", same_pair_args, 2), integer)));
  check("register partially concrete pair method",
        register_function(
            env, "base.pairs.int_get", "get", "pairs", "base", TH_ENV_PUBLIC,
            fn1(types, th_apply(types, "Pair", first_int_pair_args, 2),
                integer)));

  th_type *map_trait_receiver = th_var(types);
  check("register trait for overlapping map receiver patterns",
        th_env_register_trait(
            env, "MapReadable", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "map_get",
                          fn1(types, map_trait_receiver, integer))));
  th_arg int_value_map_args[] = {
      {.kind = TH_ARG_TYPE, .type = integer},
      {.kind = TH_ARG_TYPE, .type = th_var(types)},
  };
  th_arg text_value_map_args[] = {
      {.kind = TH_ARG_TYPE, .type = th_var(types)},
      {.kind = TH_ARG_TYPE, .type = string},
  };
  check("register first partially concrete map method",
        register_function(env, "base.maps.int_get", "map_get", "maps-int",
                          "base", TH_ENV_PUBLIC,
                          fn1(types, th_apply(types, "Map",
                                              int_value_map_args, 2),
                              integer)));
  check("register second incomparable map method",
        register_function(env, "base.maps.text_get", "map_get", "maps-text",
                          "base", TH_ENV_PUBLIC,
                          fn1(types, th_apply(types, "Map",
                                              text_value_map_args, 2),
                              integer)));

  th_type *integer_text_receiver = th_var(types);
  check("register integer text trait",
        th_env_register_trait(
            env, "IntegerText", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "show",
                          fn1(types, integer_text_receiver, string))));
  check("register wider signed-integer dispatch candidates",
        register_function(env, "base.int16.show", "show", "int16", "base",
                          TH_ENV_PUBLIC,
                          fn1(types, th_bitint(types, 16, true), string)) &&
            register_function(env, "base.int32.show", "show", "int32", "base",
                              TH_ENV_PUBLIC,
                              fn1(types, th_bitint(types, 32, true), string)));

  th_type *index_receiver = th_var(types);
  check("register built-in read-index trait contract",
        th_env_register_trait(
            env, "Index_Load", "builtins", "base", TH_ENV_PUBLIC,
            method_record(types, "load",
                          fn2(types, index_receiver, integer, string))));
  th_param store_params[] = {
      {.mode = TH_IN, .type = th_var(types)},
      {.mode = TH_IN, .type = integer},
      {.mode = TH_IN, .type = string},
  };
  check("register built-in write-index trait contract",
        th_env_register_trait(
            env, "Index_Store", "builtins", "base", TH_ENV_PUBLIC,
            method_record(types, "store",
                          th_func(types, integer, store_params, 3, nullptr,
                                  nullptr, 0))));
  th_type *int_array = th_array(types, integer);
  check("register ordinary read/write index implementations",
        register_function(env, "base.array.load", "load", "array", "base",
                          TH_ENV_PUBLIC,
                          fn2(types, int_array, integer, string)) &&
            register_function(env, "base.array.store", "store", "array",
                              "base", TH_ENV_PUBLIC,
                              th_func(types, integer,
                                      (th_param[]){{TH_IN, int_array},
                                                   {TH_IN, integer},
                                                   {TH_IN, string}},
                                      3, nullptr, nullptr, 0)));

  th_type *add_receiver = th_var(types);
  check("register overloaded addition trait contract",
        th_env_register_trait(
            env, "Add", "builtins", "base", TH_ENV_PUBLIC,
            method_record(types, "add",
                          fn2(types, add_receiver, add_receiver,
                              add_receiver))));
  check("register ordinary integer addition function",
        register_function(env, "base.int.add", "add", "int", "base",
                          TH_ENV_PUBLIC, fn2(types, integer, integer, integer)));

  th_type *fallible_receiver = th_var(types);
  th_type *read_error = th_name(types, "ReadError");
  check("register trait success and failure channels",
        th_env_register_trait_channels(
            env, "FallibleRead", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "fallible_read",
                          fn1(types, fallible_receiver, string)),
            method_record(types, "fallible_read", read_error)) &&
            th_env_register_function_channels(
                env, "base.reads.int_read", "fallible_read", "reads",
                "base", TH_ENV_PUBLIC, fn1(types, integer, string),
                read_error) &&
            th_env_register_function_channels(
                env, "base.reads.wrong_error", "fallible_read", "reads",
                "base", TH_ENV_PUBLIC, fn1(types, integer, string),
                th_name(types, "WriteError")));

  th_type *correlated_channel_var = th_var(types);
  th_type *correlated_contract = th_generalize(types,
      th_env_signature_contract(types,
          fn1(types, correlated_channel_var, correlated_channel_var),
          correlated_channel_var));
  th_type *wrong_correlated_contract = th_env_signature_contract(
      types, fn1(types, integer, integer), string);
  check("register jointly quantified trait and function channel contracts",
        th_env_register_trait_contracts(
            env, "CorrelatedFailure", "traits", "base", TH_ENV_PUBLIC,
            method_record(types, "correlated", correlated_contract)) &&
            th_env_register_function_contract(
                env, "base.reads.correlated_wrong", "correlated", "reads",
                "base", TH_ENV_PUBLIC, wrong_correlated_contract));

  th_type *iter_source = th_var(types);
  th_type *iter_cursor = th_var(types);
  th_type *iter_error = th_var(types);
  th_param iter_receiver = {.mode = TH_IN, .type = iter_source};
  th_type *iter_signature = th_func_with_error(
      types, iter_cursor, iter_error, &iter_receiver, 1, nullptr, nullptr, 0);
  th_member iter_method = {
      .name = "iter",
      .type = th_generalize(types, iter_signature),
  };
  th_type *iter_contracts = th_record(types, &iter_method, 1);
  th_arg optional_int_arg = {.kind = TH_ARG_TYPE, .type = integer};
  th_type *optional_int = th_apply(types, "option", &optional_int_arg, 1);
  th_param iter_impl_receiver = {.mode = TH_IN, .type = optional_int};
  th_type *iter_impl_signature = th_func_with_error(
      types, optional_int, th_name(types, "nil"), &iter_impl_receiver, 1,
      nullptr, nullptr, 0);
  th_type *iter_impl_contract = th_env_signature_contract(
      types, iter_impl_signature, th_name(types, "nil"));
  check("register iterable trait with an independent cursor and failure type",
        iter_contracts && th_env_register_trait_contracts(
            env, "IterableTest", "builtins", "base", TH_ENV_PUBLIC,
            iter_contracts));
  check("register a concrete iterable method candidate",
        iter_impl_contract && th_env_register_function_contract(
            env, "base.iterables.option_int", "iter", "iterables", "base",
            TH_ENV_PUBLIC, iter_impl_contract));

  th_type *private_get_receiver = th_var(types);
  check("register private trait method for requirement visibility",
        th_env_register_trait(
            env, "SecretGetter", "private_traits", "base", TH_ENV_PRIVATE,
            method_record(types, "get",
                          fn1(types, private_get_receiver, integer))));

  check("freeze environment", th_env_freeze(env));

  th_ctx *inside = th_ctx_new();
  th_ctx *outside = th_ctx_new();
  check("attach contexts to frozen environment",
        th_ctx_set_environment(inside, env, "consumer", "base") &&
            th_ctx_set_environment(inside, env, "consumer", "base") &&
            th_ctx_set_environment(outside, env, "consumer", "other") &&
            !th_ctx_set_environment(inside, env, "other", "base"));
  th_env_method_t first_echo = {0}, second_echo = {0};
  const th_param *first_echo_arg = nullptr, *second_echo_arg = nullptr;
  bool independent_echo_signatures =
      th_env_method_resolve(inside, integer, "ReturnsArgument", "echo",
                            &first_echo) == TH_ENV_METHOD_FOUND &&
      th_env_method_resolve(inside, integer, "ReturnsArgument", "echo",
                            &second_echo) == TH_ENV_METHOD_FOUND;
  first_echo_arg = independent_echo_signatures
      ? th_func_param(first_echo.signature, 1) : nullptr;
  second_echo_arg = independent_echo_signatures
      ? th_func_param(second_echo.signature, 1) : nullptr;
  check("each method resolution owns a fresh generic signature instance",
        independent_echo_signatures && first_echo_arg && second_echo_arg &&
            first_echo.signature != second_echo.signature &&
            th_unify_in_place(inside, first_echo_arg->type,
                              th_ind(inside, 0, integer)) &&
            th_unify_in_place(inside, second_echo_arg->type,
                              th_ind(inside, 0, string)));
  th_ctx *secret_context = th_ctx_new();
  check("private type remains visible in its defining module",
        th_ctx_set_environment(secret_context, env, "secrets", "base") &&
            th_env_type_visible(secret_context, "Secret"));
  check("type visibility follows private and package scopes",
        th_env_type_visible(inside, "int") &&
            !th_env_type_visible(inside, "Secret") &&
            th_env_type_visible(inside, "PackageType") &&
            !th_env_type_visible(outside, "PackageType"));
  check("shared type context can change module visibility view",
        th_ctx_set_environment_scope(outside, "secrets", "base") &&
            th_env_type_visible(outside, "Secret") &&
            th_ctx_set_environment_scope(outside, "consumer", "other") &&
            !th_env_type_visible(outside, "Secret"));

  th_type *consumer_int = th_name(inside, "int");
  th_env_method_t selected = {0};
  th_arg optional_consumer_int_arg = {
      .kind = TH_ARG_TYPE,
      .type = consumer_int,
  };
  th_type *optional_consumer_int =
      th_apply(inside, "option", &optional_consumer_int_arg, 1);
  th_env_method_t iterator_method = {0};
  check("independent trait result and generic failure channels resolve",
        th_env_method_candidates(inside, "IterableTest", "iter", nullptr,
                                 0) == 1 &&
            th_env_method_lookup(inside, optional_consumer_int,
                                 "IterableTest", "iter", &iterator_method) &&
            !strcmp(iterator_method.qualified_name,
                    "base.iterables.option_int"));
  th_type *readable_variable = th_var(inside);
  check("record generic trait obligation on an unresolved variable",
        th_var_constrain(
            inside, readable_variable,
            (th_constraint){.kind = TH_CON_IMPLEMENTS, .iface = "Readable"}));
  const char *variable_traits[2] = {0};
  check("unresolved trait obligations remain visible to inference",
        th_implements(inside, readable_variable, "Readable") &&
            th_infer_ifaces(inside, readable_variable, variable_traits, 2) ==
                1 &&
            !strcmp(variable_traits[0], "Readable"));
  check("unresolved trait obligations do not trigger concrete dispatch",
        th_env_method_resolve(inside, readable_variable, "Readable", "read",
                              &selected) == TH_ENV_METHOD_NOT_FOUND);
  check("trait obligation accepts a matching concrete type",
        th_unify(inside, readable_variable, consumer_int) != nullptr);
  th_type *invalid_readable_variable = th_var(inside);
  th_var_constrain(
      inside, invalid_readable_variable,
      (th_constraint){.kind = TH_CON_IMPLEMENTS, .iface = "Readable"});
  check("trait obligation rejects a concrete type without a matching method",
        th_unify(inside, invalid_readable_variable, th_name(inside, "string"))
            == nullptr);
  check("ordinary function candidate supplies open-trait fact",
        th_implements(inside, consumer_int, "Readable"));
  th_env_method_t fallible_candidates[2] = {0};
  th_env_method_t fallible_method = {0};
  check("trait candidate matching checks its separately tracked failure type",
        th_env_method_candidates(inside, "FallibleRead", "fallible_read",
                                 fallible_candidates, 2) == 1 &&
            !strcmp(fallible_candidates[0].qualified_name,
                    "base.reads.int_read") &&
            th_env_method_lookup(inside, consumer_int, "FallibleRead",
                                 "fallible_read", &fallible_method) &&
            th_unify(inside, fallible_method.error_type, read_error) &&
            th_implements(inside, consumer_int, "FallibleRead"));
  check("trait channel matching preserves shared quantified variables",
        !th_implements(inside, consumer_int, "CorrelatedFailure") &&
            th_env_method_candidates(inside, "CorrelatedFailure",
                                     "correlated", nullptr, 0) == 0);
  check("package-scoped candidate is invisible outside package",
        !th_implements(outside, th_name(outside, "int"), "Readable"));
  check("dispatch generation can enumerate viable method candidates",
        th_env_method_candidates(inside, "Readable", "read", nullptr, 0) == 3 &&
            th_env_method_candidates(outside, "Readable", "read", nullptr, 0) ==
                0);
  th_env_method_requirement_t get_requirements[3] = {0};
  size_t get_requirement_count = th_env_method_requirements(
      inside, "get", get_requirements, 3);
  check("member inference can enumerate visible trait contracts by method",
        get_requirement_count == 2 &&
            !strcmp(get_requirements[0].trait_name, "BoxReadable") &&
            !strcmp(get_requirements[1].trait_name, "PairReadable") &&
            th_env_method_requirements(outside, "get", nullptr, 0) == 2);
  th_type *box_requirement_receiver = get_requirement_count
      ? (th_type *)th_func_param(get_requirements[0].signature, 0)->type
      : nullptr;
  th_type *box_requirement_result = get_requirement_count
      ? th_func_ret(get_requirements[0].signature) : nullptr;
  th_type *box_int = th_apply(
      inside, "Box",
      &(th_arg){.kind = TH_ARG_TYPE, .type = th_name(inside, "int")}, 1);
  check("method requirement keeps receiver and result variables related",
        box_requirement_receiver && box_requirement_result &&
            th_unify_in_place(inside, box_requirement_receiver, box_int) &&
            th_unify_in_place(inside, box_requirement_result, box_int));
  th_env_method_requirement_t repeated_requirements[2] = {0};
  size_t repeated_count = th_env_method_requirements(
      inside, "get", repeated_requirements, 2);
  th_type *repeated_receiver = repeated_count
      ? (th_type *)th_func_param(repeated_requirements[0].signature, 0)->type
      : nullptr;
  check("each method requirement query has independent type variables",
        repeated_count == 2 && repeated_receiver &&
            th_unify_in_place(inside, repeated_receiver,
                              th_name(inside, "string")));
  check("all methods are required for a trait fact",
        !th_implements(inside, consumer_int, "Paired"));
  check("method dispatch requires the complete trait contract",
        !th_env_method_lookup(inside, consumer_int, "Paired", "first",
                              &selected));

  check("method lookup returns method candidate identity",
        th_env_method_lookup(inside, consumer_int, "Readable", "read",
                             &selected) &&
            !strcmp(selected.qualified_name, "base.ints.read_first"));
  check("type module candidate outranks other package modules",
        th_env_method_lookup(inside, consumer_int, "Readable", "read",
                             &selected) &&
            !strcmp(selected.module, "ints"));

  check(
      "most specific function candidate wins dispatch",
      th_env_method_lookup(inside, box_int, "BoxReadable", "get", &selected) &&
          !strcmp(selected.qualified_name, "base.box.int_get"));
  th_type *box_pattern_item = th_var(inside);
  th_arg box_pattern_arg = {
      .kind = TH_ARG_TYPE,
      .type = box_pattern_item,
  };
  th_type *box_pattern = th_apply(inside, "Box", &box_pattern_arg, 1);
  check("structural type-pattern subsumption distinguishes generic receivers",
        box_pattern && th_env_type_pattern_subsumes(box_pattern, box_int) &&
            !th_env_type_pattern_subsumes(box_int, box_pattern) &&
            th_env_type_pattern_subsumes(box_int, box_int));
  const char *selected_trait = nullptr;
  check("method-name lookup selects the applicable visible trait contract",
        th_env_method_resolve_by_name(inside, box_int, "get", &selected,
                                      &selected_trait) ==
                TH_ENV_METHOD_FOUND &&
            !strcmp(selected.qualified_name, "base.box.int_get") &&
            !strcmp(selected_trait, "BoxReadable"));
  th_env_vtable_slot_t box_slots[1] = {0};
  size_t box_slot_count = 0;
  th_env_vtable_plan_t box_vtable = {0};
  check("concrete trait values receive a complete vtable plan",
        th_env_trait_vtable_plan(inside, "BoxReadable", box_int, box_slots,
                                 1, &box_slot_count, &box_vtable) ==
                TH_ENV_METHOD_FOUND &&
            box_slot_count == 1 &&
            box_vtable.concrete_type == box_int &&
            box_vtable.concrete_type_hash == th_hash(inside, box_int) &&
            box_vtable.slot_count == 1 &&
            box_vtable.required_slot_count == 1 &&
            !strcmp(box_slots[0].method_name, "get") &&
            !strcmp(box_slots[0].implementation.qualified_name,
                    "base.box.int_get"));
  th_env_vtable_slot_t missing_slots[2] = {
      {.method_name = "unchanged"}, {.method_name = "unchanged"}};
  size_t missing_slot_count = 7;
  check("incomplete trait implementations cannot produce partial vtables",
        th_env_trait_vtable_plan(inside, "Paired", consumer_int,
                                 missing_slots, 2, &missing_slot_count,
                                 nullptr) ==
                TH_ENV_METHOD_NOT_FOUND &&
            missing_slot_count == 0 &&
            !strcmp(missing_slots[0].method_name, "unchanged") &&
            !strcmp(missing_slots[1].method_name, "unchanged"));
  th_arg concrete_pair_args[] = {
      {.kind = TH_ARG_TYPE, .type = th_name(inside, "int")},
      {.kind = TH_ARG_TYPE, .type = th_name(inside, "int")},
  };
  th_type *concrete_pair = th_apply(inside, "Pair", concrete_pair_args, 2);
  th_type *scheme_receiver = th_var(inside);
  th_type *scheme_result = th_var(inside);
  check("retain a deferred method result on an unresolved receiver",
        th_var_constrain(
            inside, scheme_receiver,
            (th_constraint){.kind = TH_CON_METHOD,
                            .method = {.name = "get",
                                       .result = scheme_result}}));
  th_type *deferred_scheme = th_generalize(
      inside, fn1(inside, scheme_receiver, scheme_result));
  th_type *deferred_call = th_instantiate(inside, deferred_scheme);
  th_type *deferred_receiver =
      (th_type *)th_func_param(deferred_call, 0)->type;
  th_type *deferred_result = th_func_ret(deferred_call);
  check("concrete receiver resolves the deferred method and its result",
        th_unify_in_place(inside, deferred_receiver, box_int) &&
            th_unify(inside, deferred_result, box_int));
  th_type *self_echo_receiver = th_var(inside);
  check("self-returning method constraints bind without recursive checking",
        th_var_constrain(
            inside, self_echo_receiver,
            (th_constraint){.kind = TH_CON_METHOD,
                            .method = {
                                .name = "echo",
                                .result = self_echo_receiver,
                                .call = fn0(inside, self_echo_receiver),
                            }}) &&
            th_unify_in_place(inside, self_echo_receiver, box_int));
  th_type *ambiguous_call = th_instantiate(inside, deferred_scheme);
  th_type *ambiguous_receiver =
      (th_type *)th_func_param(ambiguous_call, 0)->type;
  check("deferred method resolution rejects ambiguous concrete dispatch",
        !th_unify_in_place(inside, ambiguous_receiver, concrete_pair));
  th_type *missing_call = th_instantiate(inside, deferred_scheme);
  th_type *missing_receiver =
      (th_type *)th_func_param(missing_call, 0)->type;
  check("deferred method resolution rejects a missing implementation",
        !th_unify_in_place(inside, missing_receiver,
                           th_name(inside, "string")));
  th_ctx *late_environment_context = th_ctx_new();
  th_type *late_receiver = th_var(late_environment_context);
  th_type *late_result = th_var(late_environment_context);
  th_var_constrain(late_environment_context, late_receiver,
                   (th_constraint){.kind = TH_CON_METHOD,
                                   .method = {.name = "get",
                                              .result = late_result}});
  th_arg late_box_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(late_environment_context, "int"),
  };
  th_type *late_box = th_apply(late_environment_context, "Box",
                               &late_box_arg, 1);
  bool bound_before_environment = th_unify_in_place(
      late_environment_context, late_receiver, late_box) != nullptr;
  check("environment attachment validates previously bound method obligations",
        bound_before_environment &&
            th_ctx_set_environment(late_environment_context, env, "consumer",
                                   "base") &&
            th_unify(late_environment_context, late_result,
                     late_box));
  th_ctx *late_invalid_context = th_ctx_new();
  th_type *late_invalid_receiver = th_var(late_invalid_context);
  th_var_constrain(late_invalid_context, late_invalid_receiver,
                   (th_constraint){.kind = TH_CON_METHOD,
                                   .method = {.name = "get",
                                              .result = th_var(late_invalid_context)}});
  bool invalid_bound_early = th_unify_in_place(
      late_invalid_context, late_invalid_receiver,
      th_name(late_invalid_context, "string")) != nullptr;
  check("late environment attachment reports a missing method implementation",
        invalid_bound_early &&
            !th_ctx_set_environment(late_invalid_context, env, "consumer",
                                    "base"));
  th_ctx *late_trait_context = th_ctx_new();
  th_type *late_trait_receiver = th_var(late_trait_context);
  th_var_constrain(late_trait_context, late_trait_receiver,
                   (th_constraint){.kind = TH_CON_IMPLEMENTS,
                                   .iface = "Readable"});
  bool trait_bound_before_environment = th_unify_in_place(
      late_trait_context, late_trait_receiver,
      th_name(late_trait_context, "int")) != nullptr;
  check("environment attachment validates a trait bound after early binding",
        trait_bound_before_environment &&
            th_ctx_set_environment(late_trait_context, env, "consumer",
                                   "base"));
  th_ctx *late_invalid_trait_context = th_ctx_new();
  th_type *late_invalid_trait_receiver = th_var(late_invalid_trait_context);
  th_var_constrain(late_invalid_trait_context, late_invalid_trait_receiver,
                   (th_constraint){.kind = TH_CON_IMPLEMENTS,
                                   .iface = "Readable"});
  bool invalid_trait_bound_early = th_unify_in_place(
      late_invalid_trait_context, late_invalid_trait_receiver,
      th_name(late_invalid_trait_context, "string")) != nullptr;
  check("environment attachment rejects a failed trait bound after binding",
        invalid_trait_bound_early &&
            !th_ctx_set_environment(late_invalid_trait_context, env,
                                    "consumer", "base"));
  th_ctx *scoped_method_context = th_ctx_new();
  th_type *scoped_method_receiver = th_var(scoped_method_context);
  th_type *scoped_method_result = th_var(scoped_method_context);
  bool scoped_method_added = th_ctx_set_scope(
      scoped_method_context, "consumer", "base") &&
      th_var_constrain(scoped_method_context, scoped_method_receiver,
          (th_constraint){.kind = TH_CON_METHOD,
              .method = {.name = "read", .result = scoped_method_result}});
  bool scoped_method_bound = scoped_method_added &&
      th_unify_in_place(scoped_method_context, scoped_method_receiver,
                        th_name(scoped_method_context, "int"));
  check("deferred method obligations retain their authoring package",
        scoped_method_bound &&
            th_ctx_set_environment(scoped_method_context, env, "consumer",
                                   "other") &&
            th_unify(scoped_method_context, scoped_method_result,
                     th_name(scoped_method_context, "int")));
  th_ctx *scoped_trait_context = th_ctx_new();
  th_type *scoped_trait_receiver = th_var(scoped_trait_context);
  bool scoped_trait_added = th_ctx_set_scope(
      scoped_trait_context, "consumer", "base") &&
      th_var_constrain(scoped_trait_context, scoped_trait_receiver,
          (th_constraint){.kind = TH_CON_IMPLEMENTS, .iface = "Readable"});
  bool scoped_trait_bound = scoped_trait_added &&
      th_unify_in_place(scoped_trait_context, scoped_trait_receiver,
                        th_name(scoped_trait_context, "int"));
  check("deferred trait obligations retain their authoring package",
        scoped_trait_bound &&
            th_ctx_set_environment(scoped_trait_context, env, "consumer",
                                   "other"));
  th_ctx *wrong_scoped_trait_context = th_ctx_new();
  th_type *wrong_scoped_trait_receiver = th_var(wrong_scoped_trait_context);
  bool wrong_scope_added = th_ctx_set_scope(
      wrong_scoped_trait_context, "consumer", "other") &&
      th_var_constrain(wrong_scoped_trait_context, wrong_scoped_trait_receiver,
          (th_constraint){.kind = TH_CON_IMPLEMENTS, .iface = "Readable"});
  bool wrong_scope_bound = wrong_scope_added &&
      th_unify_in_place(wrong_scoped_trait_context,
                        wrong_scoped_trait_receiver,
                        th_name(wrong_scoped_trait_context, "int"));
  check("deferred trait obligations do not inherit a later package scope",
        wrong_scope_bound &&
            !th_ctx_set_environment(wrong_scoped_trait_context, env,
                                    "consumer", "base"));
  th_ctx_free(wrong_scoped_trait_context);
  th_ctx_free(scoped_trait_context);
  th_ctx_free(scoped_method_context);
  th_ctx_free(late_invalid_trait_context);
  th_ctx_free(late_trait_context);
  th_ctx_free(late_invalid_context);
  th_ctx_free(late_environment_context);
  check("integer receiver promotes to a matching trait method",
        th_env_method_lookup(inside, th_bitint(inside, 8, true),
                             "IntegerText", "show", &selected) &&
            !strcmp(selected.qualified_name, "base.int16.show"));
  check("least-widening integer receiver candidate wins dispatch",
        th_env_method_lookup(inside, th_bitint(inside, 16, true),
                             "IntegerText", "show", &selected) &&
            !strcmp(selected.qualified_name, "base.int16.show"));
  th_type *consumer_int_array = th_array(inside, consumer_int);
  check("read indexing resolves through the ordinary trait path",
        th_env_implements(inside, consumer_int_array, "Index_Load") &&
            th_env_method_lookup(inside, consumer_int_array, "Index_Load",
                                 "load", &selected) &&
            !strcmp(selected.qualified_name, "base.array.load"));
  check("write indexing resolves independently through its trait",
        th_env_implements(inside, consumer_int_array, "Index_Store") &&
            th_env_method_lookup(inside, consumer_int_array, "Index_Store",
                                 "store", &selected) &&
            !strcmp(selected.qualified_name, "base.array.store"));
  check("overloaded integer addition uses the same trait dispatch path",
        th_env_method_lookup(inside, consumer_int, "Add", "add", &selected) &&
            !strcmp(selected.qualified_name, "base.int.add") &&
            !th_env_implements(inside, th_name(inside, "string"), "Add"));
  th_env_method_t box_dispatch_candidates[2] = {0};
  size_t box_dispatch_count = th_env_method_candidates(
      inside, "BoxReadable", "get", box_dispatch_candidates, 2);
  check("trait-value dispatch enumerates generic and concrete candidates",
        box_dispatch_count == 2 &&
            ((!strcmp(box_dispatch_candidates[0].qualified_name,
                      "base.box.generic_get") &&
              !strcmp(box_dispatch_candidates[1].qualified_name,
                      "base.box.int_get")) ||
             (!strcmp(box_dispatch_candidates[1].qualified_name,
                      "base.box.generic_get") &&
             !strcmp(box_dispatch_candidates[0].qualified_name,
                      "base.box.int_get"))));
  th_arg dispatch_string_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(inside, "string"),
  };
  th_type *box_string =
      th_apply(inside, "Box", &dispatch_string_arg, 1);
  th_type *possible_box_receivers[] = {box_int, box_string};
  th_env_dispatch_entry_t dispatch_entries[2] = {0};
  size_t dispatch_count = 0;
  bool direct_box_call = true;
  th_env_method_status_t dispatch_status = th_env_method_plan_dispatch(
      inside, "BoxReadable", "get", possible_box_receivers, 2,
      dispatch_entries, 2, &dispatch_count, &direct_box_call);
  check("known trait-value receiver types produce a complete runtime plan",
        dispatch_status == TH_ENV_METHOD_FOUND && dispatch_count == 2 &&
            !direct_box_call &&
            dispatch_entries[0].receiver_type == box_int &&
            dispatch_entries[1].receiver_type == box_string &&
            !strcmp(dispatch_entries[0].method.qualified_name,
                    "base.box.int_get") &&
            !strcmp(dispatch_entries[1].method.qualified_name,
                    "base.box.generic_get"));
  th_env_dispatch_entry_t static_entry = {0};
  bool direct_box_int = false;
  check("a single known runtime receiver reduces to a direct call",
        th_env_method_plan_dispatch(inside, "BoxReadable", "get", &box_int,
                                    1, &static_entry, 1, &dispatch_count,
                                    &direct_box_int) == TH_ENV_METHOD_FOUND &&
            dispatch_count == 1 && direct_box_int &&
            !strcmp(static_entry.method.qualified_name,
                    "base.box.int_get"));
  th_env_dispatch_entry_t truncated_entry = {0};
  size_t full_dispatch_count = 0;
  check("dispatch plans report required capacity without losing order",
        th_env_method_plan_dispatch(
            inside, "BoxReadable", "get", possible_box_receivers, 2,
            &truncated_entry, 1, &full_dispatch_count, nullptr) ==
                TH_ENV_METHOD_FOUND &&
            full_dispatch_count == 2 &&
            truncated_entry.receiver_type == box_int &&
            !strcmp(truncated_entry.method.qualified_name,
                    "base.box.int_get"));
  th_type *unsupported_receiver = th_name(inside, "string");
  th_env_dispatch_entry_t absent_entry = {0};
  size_t absent_count = 99;
  check("a missing implementation rejects dispatch planning atomically",
        th_env_method_plan_dispatch(inside, "BoxReadable", "get",
                                    &unsupported_receiver, 1, &absent_entry, 1,
                                    &absent_count, nullptr) ==
                TH_ENV_METHOD_NOT_FOUND &&
            absent_count == 0 && !absent_entry.method.qualified_name);
  th_env_dispatch_entry_t untouched_entry = {0};
  size_t rejected_count = 99;
  check("ambiguous runtime receiver types reject the whole dispatch plan",
        th_env_method_plan_dispatch(inside, "PairReadable", "get",
                                    &concrete_pair, 1, &untouched_entry, 1,
                                    &rejected_count, nullptr) ==
                TH_ENV_METHOD_AMBIGUOUS &&
            rejected_count == 0 && !untouched_entry.method.qualified_name);

  check("incomparable overlapping methods are reported as ambiguous",
        th_env_method_resolve(inside, concrete_pair, "PairReadable", "get",
                              &selected) == TH_ENV_METHOD_AMBIGUOUS);
  th_arg concrete_map_args[] = {
      {.kind = TH_ARG_TYPE, .type = th_name(inside, "int")},
      {.kind = TH_ARG_TYPE, .type = th_name(inside, "string")},
  };
  th_type *concrete_map = th_apply(inside, "Map", concrete_map_args, 2);
  check("generic map receiver satisfies both overlapping candidates",
        th_env_implements(inside, concrete_map, "MapReadable"));
  check("incomparable map receiver patterns are ambiguous",
        th_env_method_resolve(inside, concrete_map, "MapReadable", "map_get",
                              &selected) == TH_ENV_METHOD_AMBIGUOUS);
  check("method-name lookup propagates ambiguity across trait contracts",
        th_env_method_resolve_by_name(inside, concrete_pair, "get", &selected,
                                      &selected_trait) ==
            TH_ENV_METHOD_AMBIGUOUS);

  enum { QUERY_THREADS = 8 };
  pthread_t query_threads[QUERY_THREADS];
  environment_query_run_t query_runs[QUERY_THREADS];
  bool query_created[QUERY_THREADS] = {0};
  for (size_t i = 0; i < QUERY_THREADS; i++) {
    query_runs[i] = (environment_query_run_t){.environment = env};
    query_created[i] =
        pthread_create(&query_threads[i], nullptr, query_frozen_environment,
                       &query_runs[i]) == 0;
  }
  bool concurrent_queries_ok = true;
  bool concurrent_composition_ok = true;
  for (size_t i = 0; i < QUERY_THREADS; i++) {
    if (!query_created[i]) {
      concurrent_queries_ok = false;
      concurrent_composition_ok = false;
      continue;
    }
    pthread_join(query_threads[i], nullptr);
    concurrent_queries_ok &= query_runs[i].ok;
    concurrent_composition_ok &= query_runs[i].composed_ok;
  }
  check("frozen trait facts support concurrent independent queries",
        concurrent_queries_ok);
  check("independent environments compose concurrently from frozen inputs",
        concurrent_composition_ok);

  th_type *unknown = th_var(inside);
  check("open receiver query stays unresolved and unbound",
        !th_env_implements(inside, unknown, "Readable") &&
            th_resolve(unknown) == unknown);

  th_type *constrained = th_var(inside);
  th_constraint obligation = {
      .kind = TH_CON_IMPLEMENTS,
      .iface = "Readable",
  };
  th_var_constrain(inside, constrained, obligation);
  check("trait obligation resolves from function-derived facts",
      th_unify_in_place(inside, constrained, consumer_int) != nullptr);

  th_env *base_library = th_env_new();
  th_ctx *base_library_types = th_ctx_new();
  th_type *fetch_receiver = th_var(base_library_types);
  th_type *string_type = th_name(base_library_types, "string");
  th_type *fetch_argument_type = th_name(base_library_types, "int");
  th_type *link_generic_element = th_var(base_library_types);
  th_type *deferred_fetch_receiver = th_var(base_library_types);
  th_type *deferred_fetch_result = th_var(base_library_types);
  th_param deferred_fetch_argument = {
      .mode = TH_IN,
      .type = fetch_argument_type,
  };
  th_type *deferred_fetch_call = th_func(
      base_library_types, deferred_fetch_result, &deferred_fetch_argument, 1,
      nullptr, nullptr, 0);
  th_type *fetch_error_type = th_name(base_library_types, "FetchError");
  bool deferred_fetch_constrained = th_ctx_set_scope(
      base_library_types, "traits", "base") && th_var_constrain(
      base_library_types, deferred_fetch_receiver,
      (th_constraint){.kind = TH_CON_METHOD,
                      .method = {.name = "fetch",
                                 .result = deferred_fetch_result,
                                 .call = deferred_fetch_call}});
  th_arg link_generic_box_arg = {
      .kind = TH_ARG_TYPE,
      .type = link_generic_element,
  };
  const uint8_t box_layout[] = {1, 8, 8, 0};
  const uint8_t fetch_trait_abi[] = {1, 2, 0};
  const uint8_t generic_fetch_abi[] = {2, 1, 1};
  const uint8_t global_semantics[] = {1, 3, 0};
  th_type *marker_methods = th_record(base_library_types, nullptr, 0);
  th_type *bracket_element = th_var(base_library_types);
  th_type *array_element = th_var(base_library_types);
  th_type *brace_element = th_var(base_library_types);
  th_type *dict_key = th_var(base_library_types);
  th_type *dict_value = th_var(base_library_types);
  th_arg list_pattern_arg = {.kind = TH_ARG_TYPE, .type = bracket_element};
  th_arg set_pattern_arg = {.kind = TH_ARG_TYPE, .type = brace_element};
  th_arg dict_pattern_args[] = {
      {.kind = TH_ARG_TYPE, .type = dict_key},
      {.kind = TH_ARG_TYPE, .type = dict_value},
  };
  check("register builtin literal-shape marker traits",
        th_env_register_trait_contracts(
            base_library, "Bracket_Literal", "builtins", "base",
            TH_ENV_PUBLIC, marker_methods) &&
            th_env_register_trait_contracts(
                base_library, "Brace_Literal", "builtins", "base",
                TH_ENV_PUBLIC, marker_methods));
  check("register intrinsic collection-shape type facts",
        th_env_register_intrinsic_type_pattern(
            base_library, "Bracket_Literal",
            th_apply(base_library_types, "list", &list_pattern_arg, 1)) &&
            th_env_register_intrinsic_type_pattern(
                base_library, "Bracket_Literal",
                th_array(base_library_types, array_element)) &&
            th_env_register_intrinsic_type_pattern(
                base_library, "Brace_Literal",
                th_apply(base_library_types, "set", &set_pattern_arg, 1)) &&
            th_env_register_intrinsic_type_pattern(
                base_library, "Brace_Literal",
                th_apply(base_library_types, "dict", dict_pattern_args, 2)));
  th_type *same_trait_receiver = th_var(base_library_types);
  th_type *bound_trait_receiver = th_var(base_library_types);
  th_type *bound_value = th_bound(base_library_types, 1, 0);
  check("register persisted Box type",
        th_env_register_type(base_library, "Box", "types", "base",
                             TH_ENV_PUBLIC));
  th_member box_shape_member = {
      .name = "value",
      .type = th_name(base_library_types, "int"),
  };
  th_type *box_shape = th_record(base_library_types, &box_shape_member, 1);
  check("register a structural definition for a named type",
        box_shape && th_env_register_type_shape(
            base_library, "Box", TH_ENV_TYPE_SHAPE_RECORD, box_shape));
  check("register persisted PrivateBox type",
        th_env_register_type(base_library, "PrivateBox", "private_types",
                             "base", TH_ENV_PRIVATE));
  check("register persisted Fetch trait",
        th_env_register_trait_channels(
            base_library, "Fetch", "traits", "base", TH_ENV_PUBLIC,
            method_record(base_library_types, "fetch",
                          fn2(base_library_types, fetch_receiver,
                              fetch_argument_type, string_type)),
            method_record(base_library_types, "fetch", fetch_error_type)));
  check("register persisted Same trait",
        th_env_register_trait(
            base_library, "Same", "traits", "base", TH_ENV_PUBLIC,
            method_record(base_library_types, "same",
                          fn1(base_library_types, same_trait_receiver,
                              same_trait_receiver))));
  check("register persisted BoundSame trait",
        th_env_register_trait(
            base_library, "BoundSame", "traits", "base", TH_ENV_PUBLIC,
            method_record(base_library_types, "bound_same",
                          fn1(base_library_types, bound_trait_receiver,
                              bound_trait_receiver))));
  check("register persisted DeferredFetch trait",
        deferred_fetch_constrained && th_env_register_trait(
            base_library, "DeferredFetch", "traits", "base", TH_ENV_PUBLIC,
            method_record(base_library_types, "probe",
                          fn1(base_library_types, deferred_fetch_receiver,
                              deferred_fetch_result))));
  check("register persisted generic Fetch implementation",
        th_env_register_function_channels(
            base_library, "base.generic.fetch", "fetch", "generic", "base",
            TH_ENV_PUBLIC,
            fn2(base_library_types,
                th_apply(base_library_types, "Box", &link_generic_box_arg, 1),
                fetch_argument_type, string_type), fetch_error_type));
  check("register persisted generic identity implementation",
        th_env_register_function(
            base_library, "base.generic.identity", "same", "generic", "base",
            TH_ENV_PUBLIC,
            th_generalize(base_library_types,
                fn1(base_library_types, link_generic_element,
                    link_generic_element))));
  check("register persisted bound identity implementation",
        th_env_register_function(
            base_library, "base.generic.bound_identity", "bound_same",
            "generic", "base", TH_ENV_PUBLIC,
            th_binder(base_library_types, 1,
                fn1(base_library_types, bound_value, bound_value))));
  check("register a typed external value declaration",
        th_env_register_value(base_library, "base.counter", "counter",
            "base", "base", TH_ENV_PUBLIC,
            th_name(base_library_types, "int")));
  check("register persisted type layout metadata",
        th_env_register_metadata(base_library, TH_ENV_METADATA_TYPE, "Box",
                                 "layout", 1, box_layout,
                                 sizeof(box_layout)));
  check("register persisted trait metadata",
        th_env_register_metadata(base_library, TH_ENV_METADATA_TRAIT, "Fetch",
                                 "dispatch", 1, fetch_trait_abi,
                                 sizeof(fetch_trait_abi)));
  check("register persisted function metadata",
        th_env_register_metadata(base_library, TH_ENV_METADATA_FUNCTION,
                                 "base.generic.fetch", "abi", 1,
                                 generic_fetch_abi,
                                 sizeof(generic_fetch_abi)));
  check("register value-owned semantic metadata",
        th_env_register_metadata(base_library, TH_ENV_METADATA_VALUE,
            "base.counter", "n00b.global", 1, global_semantics,
            sizeof(global_semantics)));
  check("freeze base library for later linking", th_env_freeze(base_library));

  th_arg live_array_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(base_library_types, "int"),
  };
  th_arg live_list_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(base_library_types, "int"),
  };
  th_arg live_set_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(base_library_types, "int"),
  };
  th_arg live_dict_args[] = {
      {.kind = TH_ARG_TYPE,
       .type = th_name(base_library_types, "string")},
      {.kind = TH_ARG_TYPE,
       .type = th_name(base_library_types, "int")},
  };
  th_type *live_array = th_array(base_library_types, live_array_arg.type);
  th_type *live_list = th_apply(base_library_types, "list",
                                &live_list_arg, 1);
  th_type *live_set = th_apply(base_library_types, "set", &live_set_arg, 1);
  th_type *live_dict = th_apply(base_library_types, "dict", live_dict_args, 2);
  th_type *live_bracket_variable = th_var(base_library_types);
  th_type *live_list_variable = th_var(base_library_types);
  th_type *live_brace_variable = th_var(base_library_types);
  th_type *live_dict_variable = th_var(base_library_types);
  bool live_pending_constraint =
      th_var_constrain(base_library_types, live_list_variable,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = "Bracket_Literal"}) &&
      th_var_constrain(base_library_types, live_bracket_variable,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = "Bracket_Literal"}) &&
      th_var_constrain(base_library_types, live_brace_variable,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = "Brace_Literal"}) &&
      th_var_constrain(base_library_types, live_dict_variable,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = "Brace_Literal"}) &&
      th_unify_in_place(base_library_types, live_list_variable, live_list) &&
      th_unify_in_place(base_library_types, live_bracket_variable,
                        live_array) &&
      th_unify_in_place(base_library_types, live_brace_variable, live_set) &&
      th_unify_in_place(base_library_types, live_dict_variable, live_dict);
  check("live intrinsic marker facts match in their shared type context",
        live_pending_constraint &&
            th_ctx_set_environment(base_library_types, base_library, "client",
                                   "base") &&
            th_env_implements(base_library_types, live_array,
                              "Bracket_Literal"));

  th_ctx *source_check_types = th_ctx_new();
  th_arg source_box_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(source_check_types, "int"),
  };
  th_env_method_t source_method = {0};
  bool source_dispatch_ok =
      th_ctx_set_environment(source_check_types, base_library, "client",
                             "base") &&
      th_env_method_lookup(source_check_types,
                           th_apply(source_check_types, "Box",
                                    &source_box_arg, 1),
                           "Fetch", "fetch", &source_method);
  check("source-built library resolves the generic method before saving",
        source_dispatch_ok &&
            !strcmp(source_method.qualified_name, "base.generic.fetch") &&
            th_unify(source_check_types, source_method.error_type,
                     th_name(source_check_types, "FetchError")));

  char image_path[] = "/tmp/lantern-th-env-XXXXXX";
  int image_fd = mkstemp(image_path);
  check("temporary environment image path is available", image_fd >= 0);
  if (image_fd >= 0) {
    close(image_fd);
    unlink(image_path);
  }
  core_object_archive_error_t image_error;
  bool environment_saved = image_fd >= 0 &&
      th_env_save(base_library, image_path, 0, &image_error) &&
      image_error == CORE_OBJECT_ARCHIVE_OK;
  check("save frozen trait/function environment and metadata",
        environment_saved);
  bool bad_trait_visibility_saved = environment_saved &&
      mutate_environment_visibility(image_path, true, UINT32_MAX);
  th_env *bad_visibility_library = bad_trait_visibility_saved
      ? th_env_load(image_path, &image_error) : nullptr;
  check("archive rejects invalid trait visibility values",
        bad_trait_visibility_saved && !bad_visibility_library &&
            image_error == CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
  bool trait_visibility_restored = environment_saved &&
      mutate_environment_visibility(image_path, true, TH_ENV_PUBLIC);
  bool bad_function_visibility_saved = trait_visibility_restored &&
      mutate_environment_visibility(image_path, false, UINT32_MAX);
  bad_visibility_library = bad_function_visibility_saved
      ? th_env_load(image_path, &image_error) : nullptr;
  check("archive rejects invalid function visibility values",
        bad_function_visibility_saved && !bad_visibility_library &&
            image_error == CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
  bool function_visibility_restored = environment_saved &&
      mutate_environment_visibility(image_path, false, TH_ENV_PUBLIC);
  bool bad_metadata_owner_saved = function_visibility_restored &&
      mutate_environment_metadata_owner(image_path, UINT32_MAX);
  bad_visibility_library = bad_metadata_owner_saved
      ? th_env_load(image_path, &image_error) : nullptr;
  check("archive rejects invalid metadata owner tags",
        bad_metadata_owner_saved && !bad_visibility_library &&
            image_error == CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
  check("restore enum bytes after malformed-image checks",
        function_visibility_restored && mutate_environment_metadata_owner(
            image_path, TH_ENV_METADATA_TYPE));
  th_env *persisted_library = th_env_load(image_path, &image_error);
  check("reload frozen environment from typed archive records",
        persisted_library && th_env_is_frozen(persisted_library) &&
            image_error == CORE_OBJECT_ARCHIVE_OK);
  th_env *image_collision = th_env_load(image_path, &image_error);
  check("a second image load restores an independent relocated environment",
        image_collision && image_collision != persisted_library &&
            th_env_is_frozen(image_collision) &&
            image_error == CORE_OBJECT_ARCHIVE_OK);

  th_ctx *persisted_check_types = th_ctx_new();
  th_type *persisted_int = th_name(persisted_check_types, "int");
  th_arg persisted_box_arg = {.kind = TH_ARG_TYPE, .type = persisted_int};
  th_type *persisted_box_int =
      th_apply(persisted_check_types, "Box", &persisted_box_arg, 1);
  th_env_method_t persisted_method = {0};
  th_env_method_t persisted_identity = {0};
  th_env_method_t persisted_bound_identity = {0};
  th_env_metadata_view_t persisted_layout = {0};
  th_env_metadata_view_t persisted_function_abi = {0};
  th_env_metadata_view_t persisted_value_metadata = {0};
  bool persisted_environment_attached = persisted_library &&
      th_ctx_set_environment(persisted_check_types, persisted_library,
                             "client", "base");
  th_env_type_shape_kind_t persisted_shape_kind = TH_ENV_TYPE_SHAPE_NONE;
  th_type *persisted_box_shape = persisted_environment_attached
      ? th_env_type_shape(persisted_check_types, "Box", &persisted_shape_kind)
      : nullptr;
  check("loaded library rebuilds trait facts and keeps generic dispatch",
        persisted_library &&
            persisted_environment_attached &&
            persisted_box_shape &&
            persisted_shape_kind == TH_ENV_TYPE_SHAPE_RECORD &&
            th_record_len(persisted_box_shape) == 1 &&
            !strcmp(th_record_member(persisted_box_shape, 0)->name, "value") &&
            th_unify(persisted_check_types,
                th_record_member(persisted_box_shape, 0)->type,
                th_name(persisted_check_types, "int")) &&
            th_env_implements(persisted_check_types, persisted_box_int,
                              "Fetch") &&
            !th_env_type_visible(persisted_check_types, "PrivateBox") &&
            th_env_method_lookup(persisted_check_types, persisted_box_int,
                                 "Fetch", "fetch", &persisted_method) &&
            !strcmp(persisted_method.qualified_name,
                    "base.generic.fetch") &&
            th_unify(persisted_check_types, persisted_method.error_type,
                     th_name(persisted_check_types, "FetchError")) &&
            source_dispatch_ok &&
            !strcmp(persisted_method.qualified_name,
                    source_method.qualified_name) &&
            th_env_method_lookup(persisted_check_types, persisted_int,
                                 "Same", "same", &persisted_identity) &&
            !strcmp(persisted_identity.qualified_name,
                    "base.generic.identity"));
  bool loaded_type_view = false, loaded_trait_view = false;
  bool loaded_function_view = false, loaded_value_view = false;
  for (size_t i = 0; persisted_library &&
       i < th_env_declaration_count(persisted_library); i++) {
    th_env_declaration_view_t declaration = {0};
    if (!th_env_declaration_at(persisted_library, i, &declaration)) continue;
    loaded_type_view |= declaration.kind == TH_ENV_DECLARATION_TYPE &&
        !strcmp(declaration.qualified_name, "Box") &&
        !strcmp(declaration.package, "base") &&
        declaration.type_shape_kind == TH_ENV_TYPE_SHAPE_RECORD &&
        declaration.type_shape == persisted_box_shape;
    loaded_trait_view |= declaration.kind == TH_ENV_DECLARATION_TRAIT &&
        !strcmp(declaration.qualified_name, "Fetch") &&
        declaration.contract != nullptr;
    loaded_function_view |=
        declaration.kind == TH_ENV_DECLARATION_FUNCTION &&
        !strcmp(declaration.qualified_name, "base.generic.fetch") &&
        !strcmp(declaration.root_name, "fetch") && declaration.contract;
    loaded_value_view |= declaration.kind == TH_ENV_DECLARATION_VALUE &&
        !strcmp(declaration.qualified_name, "base.counter") &&
        !strcmp(declaration.root_name, "counter") &&
        declaration.value_type &&
        th_hash(persisted_check_types, declaration.value_type) ==
            th_hash(persisted_check_types, persisted_int);
  }
  th_env_declaration_view_t declaration_out_of_range = {0};
  check("loaded environment enumerates stable imported declarations",
        loaded_type_view && loaded_trait_view && loaded_function_view &&
        loaded_value_view &&
        !th_env_declaration_at(persisted_library,
                               th_env_declaration_count(persisted_library),
                               &declaration_out_of_range));
  th_arg loaded_list_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(persisted_check_types, "string"),
  };
  th_arg loaded_array_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(persisted_check_types, "int"),
  };
  th_arg loaded_set_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(persisted_check_types, "int"),
  };
  th_arg loaded_dict_args[] = {
      {.kind = TH_ARG_TYPE, .type = th_name(persisted_check_types, "string")},
      {.kind = TH_ARG_TYPE, .type = th_name(persisted_check_types, "int")},
  };
  th_arg loaded_tuple_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(persisted_check_types, "int"),
  };
  check("loaded intrinsic marker facts validate collection shapes",
        persisted_library &&
            th_env_implements(persisted_check_types,
                th_apply(persisted_check_types, "list", &loaded_list_arg, 1),
                "Bracket_Literal") &&
            th_env_implements(persisted_check_types,
                th_array(persisted_check_types, loaded_array_arg.type),
                "Bracket_Literal") &&
            th_env_implements(persisted_check_types,
                th_apply(persisted_check_types, "set", &loaded_set_arg, 1),
                "Brace_Literal") &&
            th_env_implements(persisted_check_types,
                th_apply(persisted_check_types, "dict", loaded_dict_args, 2),
                "Brace_Literal") &&
            !th_env_implements(persisted_check_types,
                th_apply(persisted_check_types, "tuple", &loaded_tuple_arg, 1),
                "Bracket_Literal"));
  th_type *empty_brace_type = th_var(persisted_check_types);
  check("empty brace trait constraint accepts a linked dictionary type",
        th_var_constrain(persisted_check_types, empty_brace_type,
            (th_constraint){.kind = TH_CON_IMPLEMENTS,
                            .iface = "Brace_Literal"}) &&
            th_unify_in_place(persisted_check_types, empty_brace_type,
                th_apply(persisted_check_types, "dict", loaded_dict_args, 2)));
  th_ctx *wrong_shape_types = th_ctx_new();
  th_ctx_set_environment(wrong_shape_types, persisted_library, "client", "base");
  th_type *wrong_brace_type = th_var(wrong_shape_types);
  th_arg wrong_shape_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(wrong_shape_types, "int"),
  };
  check("empty brace trait constraint rejects an unrelated type",
        th_var_constrain(wrong_shape_types, wrong_brace_type,
            (th_constraint){.kind = TH_CON_IMPLEMENTS,
                            .iface = "Brace_Literal"}) &&
            !th_unify_in_place(wrong_shape_types, wrong_brace_type,
                th_apply(wrong_shape_types, "tuple", &wrong_shape_arg, 1)));
  th_env *literal_extension = th_env_new();
  th_ctx *literal_extension_types = th_ctx_new();
  th_type *extension_marker_methods =
      th_record(literal_extension_types, nullptr, 0);
  th_type *vector_element = th_var(literal_extension_types);
  th_arg vector_pattern_arg = {
      .kind = TH_ARG_TYPE,
      .type = vector_element,
  };
  bool literal_extension_ready =
      th_env_register_trait_contracts(
          literal_extension, "Bracket_Literal", "builtins", "base",
          TH_ENV_PUBLIC, extension_marker_methods) &&
      th_env_register_intrinsic_type_pattern(
          literal_extension, "Bracket_Literal",
          th_apply(literal_extension_types, "Vector", &vector_pattern_arg, 1)) &&
      th_env_freeze(literal_extension);
  check("register a linked builtin-shape contribution",
        literal_extension_ready);
  const th_env *literal_inputs[] = {persisted_library, literal_extension};
  th_env *literal_composed = literal_extension_ready
      ? th_env_compose(literal_inputs, 2) : nullptr;
  bool literal_composed_ready =
      literal_composed && th_env_freeze(literal_composed);
  check("compose and freeze a second contribution to a marker trait",
        literal_composed_ready);
  th_ctx *literal_composed_types = th_ctx_new();
  th_arg literal_vector_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(literal_composed_types, "string"),
  };
  th_arg literal_dict_args[] = {
      {.kind = TH_ARG_TYPE, .type = th_name(literal_composed_types, "string")},
      {.kind = TH_ARG_TYPE, .type = th_name(literal_composed_types, "int")},
  };
  bool literal_context_ready = literal_composed_ready &&
      th_ctx_set_environment(literal_composed_types, literal_composed,
                             "client", "base");
  check("attach the composed intrinsic environment", literal_context_ready);
  check("link open builtin-shape facts across matching marker traits",
        literal_context_ready &&
            th_env_implements(literal_composed_types,
                th_apply(literal_composed_types, "Vector", &literal_vector_arg,
                         1), "Bracket_Literal") &&
            th_env_implements(literal_composed_types,
                th_apply(literal_composed_types, "dict", literal_dict_args, 2),
                "Brace_Literal"));
  check("loader retains binders without an optional quantified-variable list",
        persisted_library &&
            th_env_method_lookup(persisted_check_types, persisted_int,
                                 "BoundSame", "bound_same",
                                 &persisted_bound_identity) &&
            !strcmp(persisted_bound_identity.qualified_name,
                    "base.generic.bound_identity"));
  th_env_method_requirement_t deferred_requirement = {0};
  bool deferred_requirement_loaded = persisted_library &&
      th_env_method_requirements(persisted_check_types, "probe",
                                 &deferred_requirement, 1) == 1;
  th_type *loaded_deferred_receiver = deferred_requirement_loaded
      ? (th_type *)th_func_param(deferred_requirement.signature, 0)->type
      : nullptr;
  th_type *loaded_deferred_result = deferred_requirement_loaded
      ? th_func_ret(deferred_requirement.signature) : nullptr;
  th_constraint loaded_deferred_constraint = {0};
  bool has_deferred_constraint = loaded_deferred_receiver &&
      th_var_constraint_count(loaded_deferred_receiver) == 1 &&
      th_var_constraint_at(loaded_deferred_receiver, 0,
                           &loaded_deferred_constraint) &&
      loaded_deferred_constraint.kind == TH_CON_METHOD &&
      loaded_deferred_constraint.has_scope &&
      !strcmp(loaded_deferred_constraint.scope_module, "traits") &&
      !strcmp(loaded_deferred_constraint.scope_package, "base") &&
      !strcmp(loaded_deferred_constraint.method.name, "fetch") &&
      loaded_deferred_constraint.method.result == loaded_deferred_result &&
      loaded_deferred_constraint.method.call &&
      th_func_nparams(loaded_deferred_constraint.method.call) == 1;
  check("archive preserves deferred method call arguments and result linkage",
        has_deferred_constraint &&
            th_unify_in_place(persisted_check_types,
                              loaded_deferred_receiver, persisted_box_int) &&
            th_unify(persisted_check_types, loaded_deferred_result,
                     th_name(persisted_check_types, "string")));
  th_ctx *bad_method_call_context = th_ctx_new();
  th_ctx_set_environment(bad_method_call_context, persisted_library, "client",
                         "base");
  th_type *bad_method_receiver = th_var(bad_method_call_context);
  th_type *bad_method_result = th_var(bad_method_call_context);
  th_param bad_method_argument = {
      .mode = TH_IN,
      .type = th_name(bad_method_call_context, "string"),
  };
  th_type *bad_method_call = th_func(
      bad_method_call_context, bad_method_result, &bad_method_argument, 1,
      nullptr, nullptr, 0);
  bool bad_method_obligation = th_var_constrain(
      bad_method_call_context, bad_method_receiver,
      (th_constraint){.kind = TH_CON_METHOD,
                      .method = {.name = "fetch",
                                 .result = bad_method_result,
                                 .call = bad_method_call}});
  check("method obligations reject mismatched argument types",
        bad_method_obligation &&
            !th_unify_in_place(bad_method_call_context,
                               bad_method_receiver, persisted_box_int));
  th_ctx_free(bad_method_call_context);
  th_ctx_free(wrong_shape_types);
  check("loaded image preserves type layout and function ABI metadata",
        persisted_library &&
            th_env_metadata_count(persisted_library) == 4 &&
            th_env_metadata_at(persisted_library, 0, &persisted_layout) &&
            persisted_layout.owner_kind == TH_ENV_METADATA_TYPE &&
            !strcmp(persisted_layout.owner_name, "Box") &&
            persisted_layout.version == 1 &&
            persisted_layout.size == sizeof(box_layout) &&
            !memcmp(persisted_layout.data, box_layout, sizeof(box_layout)) &&
            th_env_metadata_at(persisted_library, 2,
                               &persisted_function_abi) &&
            persisted_function_abi.owner_kind == TH_ENV_METADATA_FUNCTION &&
            !strcmp(persisted_function_abi.owner_name,
                    "base.generic.fetch") &&
            !memcmp(persisted_function_abi.data, generic_fetch_abi,
                    sizeof(generic_fetch_abi)) &&
            th_env_metadata_at(persisted_library, 3,
                               &persisted_value_metadata) &&
            persisted_value_metadata.owner_kind == TH_ENV_METADATA_VALUE &&
            !strcmp(persisted_value_metadata.owner_name, "base.counter") &&
            !memcmp(persisted_value_metadata.data, global_semantics,
                    sizeof(global_semantics)));

  th_env *conflicting_library = th_env_new();
  check("build library with a duplicate function identity",
        th_env_register_function(
            conflicting_library, "base.generic.fetch", "fetch", "other",
            "other", TH_ENV_PUBLIC,
            fn0(base_library_types, th_name(base_library_types, "int"))) &&
            th_env_freeze(conflicting_library));
  const th_env *conflicting_inputs[] = {base_library, conflicting_library};
  th_env_compose_error_t compose_error = {0};
  core_diagnostics_t *link_diagnostics = core_diagnostics_new();
  th_env *conflicted = th_env_compose_report(
      conflicting_inputs, 2, link_diagnostics, &compose_error);
  const core_diagnostic_t *link_diag = core_diagnostics_count(link_diagnostics)
                                           ? core_diagnostics_at(
                                                 link_diagnostics, 0)
                                           : nullptr;
  check("link conflict identifies the duplicate function",
        !conflicted && compose_error.status == TH_ENV_COMPOSE_FUNCTION_CONFLICT
            && !strcmp(compose_error.identity, "base.generic.fetch"));
  check("link conflict is also reported through core diagnostics",
        link_diag && link_diag->severity == CORE_DIAG_ERROR &&
            core_string_eq(link_diag->code,
                           CORE_STRING_STATIC("TH-L0004")) &&
            strstr(link_diag->message.data, "base.generic.fetch"));
  core_diagnostics_free(link_diagnostics);
  th_env_free(conflicting_library);

  th_env *metadata_conflict_library = th_env_new();
  const uint8_t alternate_box_layout[] = {1, 16, 8, 0};
  check("build library with conflicting type metadata",
        th_env_register_type(metadata_conflict_library, "Box", "types",
                             "base", TH_ENV_PUBLIC) &&
            th_env_register_metadata(metadata_conflict_library,
                                     TH_ENV_METADATA_TYPE, "Box", "layout",
                                     1, alternate_box_layout,
                                     sizeof(alternate_box_layout)) &&
            th_env_freeze(metadata_conflict_library));
  const th_env *metadata_conflict_inputs[] = {base_library,
                                               metadata_conflict_library};
  conflicted = th_env_compose_diagnose(metadata_conflict_inputs, 2,
                                       &compose_error);
  check("link conflict identifies metadata owner and key",
        !conflicted &&
            compose_error.status == TH_ENV_COMPOSE_METADATA_CONFLICT &&
            !strcmp(compose_error.identity, "Box") &&
            !strcmp(compose_error.metadata_key, "layout"));
  th_env_free(metadata_conflict_library);

  th_ctx *conflicting_shape_types = th_ctx_new();
  th_env *shape_conflict_library = th_env_new();
  th_member conflicting_shape_member = {
      .name = "other",
      .type = th_name(conflicting_shape_types, "string"),
  };
  th_type *conflicting_shape = th_record(conflicting_shape_types,
      &conflicting_shape_member, 1);
  check("build library with conflicting named type shape",
        th_env_register_type(shape_conflict_library, "Box", "types", "base",
                             TH_ENV_PUBLIC) &&
            th_env_register_type_shape(shape_conflict_library, "Box",
                TH_ENV_TYPE_SHAPE_RECORD, conflicting_shape) &&
            th_env_freeze(shape_conflict_library));
  const th_env *shape_conflict_inputs[] = {base_library,
                                           shape_conflict_library};
  conflicted = th_env_compose_diagnose(shape_conflict_inputs, 2,
                                       &compose_error);
  check("link conflict identifies incompatible named type shapes",
        !conflicted && compose_error.status == TH_ENV_COMPOSE_TYPE_CONFLICT &&
            !strcmp(compose_error.identity, "Box"));
  th_env_free(shape_conflict_library);
  th_ctx_free(conflicting_shape_types);

  const th_env *base_libraries[] = {persisted_library};
  th_env *linked_library = th_env_compose(base_libraries, 1);
  th_ctx *linked_types = th_ctx_new();
  th_type *linked_int = th_name(linked_types, "int");
  th_arg linked_box_arg = {.kind = TH_ARG_TYPE, .type = linked_int};
  th_type *linked_box_int =
      th_apply(linked_types, "Box", &linked_box_arg, 1);
  check("link module contributions into a new environment snapshot",
        linked_library &&
            th_env_register_function_channels(
                linked_library, "plugin.int_box.fetch", "fetch", "int-box",
                "plugin", TH_ENV_PUBLIC,
                fn2(linked_types, linked_box_int,
                    th_name(linked_types, "int"),
                    th_name(linked_types, "string")),
                th_name(linked_types, "FetchError")) &&
            th_env_register_metadata(linked_library,
                                     TH_ENV_METADATA_FUNCTION,
                                     "plugin.int_box.fetch", "abi", 1,
                                     generic_fetch_abi,
                                     sizeof(generic_fetch_abi)) &&
            th_env_freeze(linked_library));

  th_ctx_set_environment(linked_types, linked_library, "client", "base");
  th_env_type_shape_kind_t linked_shape_kind = TH_ENV_TYPE_SHAPE_NONE;
  th_type *linked_box_shape = th_env_type_shape(linked_types, "Box",
                                                &linked_shape_kind);
  check("composed snapshots retain canonical named type shapes",
        linked_box_shape && linked_shape_kind == TH_ENV_TYPE_SHAPE_RECORD &&
            th_record_len(linked_box_shape) == 1 &&
            !strcmp(th_record_member(linked_box_shape, 0)->name, "value"));

  th_env_metadata_view_t layout_metadata = {0};
  th_env_metadata_view_t function_metadata = {0};
  check("type, function, and value metadata survive environment composition",
        th_env_metadata_count(linked_library) == 5 &&
            th_env_metadata_at(linked_library, 0, &layout_metadata) &&
            layout_metadata.owner_kind == TH_ENV_METADATA_TYPE &&
            !strcmp(layout_metadata.owner_name, "Box") &&
            !strcmp(layout_metadata.key, "layout") &&
            layout_metadata.version == 1 &&
            layout_metadata.size == sizeof(box_layout) &&
            !memcmp(layout_metadata.data, box_layout, sizeof(box_layout)) &&
            th_env_metadata_at(linked_library, 1, &function_metadata) &&
            function_metadata.owner_kind == TH_ENV_METADATA_TRAIT &&
            !strcmp(function_metadata.owner_name, "Fetch") &&
            !strcmp(function_metadata.key, "dispatch") &&
            function_metadata.size == sizeof(fetch_trait_abi) &&
            !memcmp(function_metadata.data, fetch_trait_abi,
                    sizeof(fetch_trait_abi)) &&
            th_env_metadata_at(linked_library, 2, &function_metadata) &&
            function_metadata.owner_kind == TH_ENV_METADATA_FUNCTION &&
            !strcmp(function_metadata.owner_name, "base.generic.fetch") &&
            !strcmp(function_metadata.key, "abi") &&
            th_env_metadata_at(linked_library, 3, &function_metadata) &&
            function_metadata.owner_kind == TH_ENV_METADATA_VALUE &&
            !strcmp(function_metadata.owner_name, "base.counter") &&
            !strcmp(function_metadata.key, "n00b.global") &&
            th_env_metadata_at(linked_library, 4, &function_metadata) &&
            function_metadata.owner_kind == TH_ENV_METADATA_FUNCTION &&
            !strcmp(function_metadata.owner_name, "plugin.int_box.fetch") &&
            !strcmp(function_metadata.key, "abi"));
  th_ctx *base_consumer = th_ctx_new();
  th_ctx *linked_consumer = th_ctx_new();
  th_env_method_t base_method = {0};
  th_env_method_t linked_method = {0};
  th_arg base_box_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(base_consumer, "int"),
  };
  check("link-time additions specialize dispatch without mutating libraries",
        linked_library &&
            th_ctx_set_environment(base_consumer, base_library, "client",
                                   "base") &&
            th_ctx_set_environment(linked_consumer, linked_library, "client",
                                   "base") &&
            th_env_method_lookup(
                base_consumer,
                th_apply(base_consumer, "Box", &base_box_arg, 1), "Fetch",
                "fetch", &base_method) &&
            th_env_method_lookup(linked_consumer, linked_box_int, "Fetch",
                                 "fetch", &linked_method) &&
            !strcmp(base_method.qualified_name, "base.generic.fetch") &&
            th_unify(base_consumer, base_method.error_type,
                     th_name(base_consumer, "FetchError")) &&
            !strcmp(linked_method.qualified_name, "plugin.int_box.fetch"));

  th_ctx_free(outside);
  th_ctx_free(secret_context);
  th_ctx_free(inside);
  th_env_free(env);
  th_ctx_free(base_consumer);
  th_ctx_free(linked_consumer);
  th_env_free(linked_library);
  th_env_free(image_collision);
  th_ctx_free(linked_types);
  th_ctx_free(literal_composed_types);
  th_env_free(literal_composed);
  th_ctx_free(literal_extension_types);
  th_env_free(literal_extension);
  th_ctx_free(persisted_check_types);
  th_ctx_free(source_check_types);
  th_env_free(persisted_library);
  unlink(image_path);
  th_env_free(base_library);
  th_ctx_free(base_library_types);
  th_ctx_free(utility_types);
  th_ctx_free(types);
  return report("type environment");
}
