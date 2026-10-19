#define _XOPEN_SOURCE 700

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "th/environment.h"

#define S(x) CORE_STRING_STATIC(x)

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

static bool mutate_environment_visibility(core_string_t path, bool trait,
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

static bool mutate_environment_metadata_owner(core_string_t path,
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
      size_t fixed = group == 1 && root_record->schema_version >= 9
          ? 60 : 52;
      valid = skip_archive_strings(payload, size, &offset, strings) &&
              offset <= size && size - offset >= fixed;
      if (valid) offset += fixed;
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

static th_type *method_record(th_ctx *ctx, core_string_t name,
                              th_type *signature) {
  th_member method = {.name = name, .type = signature};
  return th_record(ctx, &method, 1);
}

static bool register_function(th_env *env, core_string_t qualified_name,
                              core_string_t root, core_string_t module,
                              core_string_t package,
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
  run->ok = th_ctx_set_environment(context, run->environment, S("consumer"),
                                   S("base"));
  for (int i = 0; run->ok && i < 100; i++) {
    th_type *integer = th_name(context, S("int"));
    th_env_method_t method = {0};
    run->ok = th_env_implements(context, integer, S("Readable")) &&
              th_env_method_candidates(context, S("Readable"), S("read"),
                                       nullptr, 0) == 3 &&
              th_env_method_resolve(context, integer, S("Readable"),
                                    S("read"), &method) ==
                  TH_ENV_METHOD_FOUND &&
              core_string_eq(method.qualified_name,
                             S("base.ints.read_first"));
  }
  th_arg integer_arg = {.kind = TH_ARG_TYPE,
                        .type = th_name(context, S("int"))};
  th_type *box_integer = th_apply(context, S("Box"), &integer_arg, 1);
  th_env_dispatch_entry_t entry = {0};
  bool direct_call = false;
  run->ok = run->ok && th_env_method_plan_dispatch(
      context, S("BoxReadable"), S("get"), &box_integer, 1, &entry, 1,
      nullptr, &direct_call) == TH_ENV_METHOD_FOUND && direct_call &&
      core_string_eq(entry.method.qualified_name, S("base.box.int_get"));

  const th_env *sources[] = {run->environment};
  th_env *composed = th_env_compose(sources, 1);
  th_ctx *composed_context = th_ctx_new();
  run->composed_ok = composed && th_env_freeze(composed) &&
      th_ctx_set_environment(composed_context, composed, S("consumer"),
                             S("base"));
  if (run->composed_ok) {
    th_type *composed_integer = th_name(composed_context, S("int"));
    th_env_method_t method = {0};
    run->composed_ok = th_env_method_resolve(
        composed_context, composed_integer, S("Readable"), S("read"),
        &method) == TH_ENV_METHOD_FOUND &&
        core_string_eq(method.qualified_name, S("base.ints.read_first"));
  }
  th_ctx_free(composed_context);
  th_env_free(composed);
  th_ctx_free(context);
  return nullptr;
}

static void test_kind_constraint_archive(void) {
  th_ctx *types = th_ctx_new();
  th_env *env = th_env_new();
  th_type *boolean = th_name(types, S("bool"));
  th_type *receiver = th_var(types);
  check("register kind-constrained trait contract",
        th_env_register_trait(env, S("SizedEqual"), S("traits"), S("base"),
            TH_ENV_PUBLIC, method_record(types, S("equal"),
                fn2(types, receiver, receiver, boolean))));
  th_type *sized = th_var(types);
  th_constraint kind = {.kind = TH_CON_KIND, .type_kind = TH_BITINT};
  check("register width-independent integer method",
        th_var_constrain(types, sized, kind)
        && register_function(env, S("base.sized.equal"), S("equal"),
            S("sized"), S("base"), TH_ENV_PUBLIC,
            th_generalize(types, fn2(types, sized, sized, boolean)))
        && th_env_register_value(env, S("base.sized.mask"), S("mask"),
                                 S("sized"), S("base"), TH_ENV_PUBLIC,
                                 th_bits(types, 8))
        && th_env_freeze(env));
  char path[] = "/tmp/lantern-kind-env-XXXXXX";
  core_string_t path_text = core_string_borrow(path, sizeof(path) - 1);
  int fd = mkstemp(path);
  if (fd >= 0) { close(fd); unlink(path); }
  core_object_archive_error_t error;
  bool saved = fd >= 0 && th_env_save(env, path_text, 0, &error);
  check("save structural kind constraints", saved);
  th_env *loaded = saved ? th_env_load(path_text, &error) : nullptr;
  const th_env *sources[] = {loaded};
  th_env *composed = loaded ? th_env_compose(sources, 1) : nullptr;
  check("compose restored kind constraints",
        composed && th_env_freeze(composed));
  const th_env *checks[] = {loaded, composed};
  for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
    th_ctx *query = th_ctx_new();
    bool ready = checks[i]
        && th_ctx_set_environment(query, checks[i], S("consumer"), S("base"));
    bool retained_bits = false;
    for (size_t j = 0; checks[i] && j < th_env_declaration_count(checks[i]);
         j++) {
      th_env_declaration_view_t view = {0};
      uint64_t width = 0;
      th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
      if (th_env_declaration_at(checks[i], j, &view)
          && view.qualified_name.data
          && core_string_eq(view.qualified_name, S("base.sized.mask")))
        retained_bits = th_bitint_info(view.value_type, &width, &flavor)
            && width == 8 && flavor == TH_BITINT_BITS;
    }
    check("bits flavor survives archive and composition", retained_bits);
    check("restored integer family accepts arbitrary signed widths",
          ready && th_env_implements(query, th_bitint(query, 7, true),
                                      S("SizedEqual")));
    check("restored integer family accepts arbitrary unsigned widths",
          ready && th_env_implements(query, th_bitint(query, 513, false),
                                      S("SizedEqual")));
    check("restored integer family rejects other type kinds",
          ready && !th_env_implements(query, th_name(query, S("string")),
                                       S("SizedEqual")));
    th_ctx_free(query);
  }
  th_env_free(composed);
  th_env_free(loaded);
  if (fd >= 0) unlink(path);
  th_env_free(env);
  th_ctx_free(types);
}

static void test_trait_type_identity(void) {
  th_ctx *types = th_ctx_new();
  th_ctx *copy_types = th_ctx_new();
  th_type *receiver = th_var(types);
  th_type *integer = th_name(types, S("int"));
  th_type *boolean = th_name(types, S("bool"));
  th_type *nil = th_name(types, S("nil"));
  th_param self = {.mode = TH_IN, .type = receiver};
  th_member methods[] = {
      {.name = S("read"), .type = th_func_with_error(types, integer, nil,
          &self, 1, nullptr, nullptr, 0)},
      {.name = S("ready"), .type = th_func_with_error(types, boolean, nil,
          &self, 1, nullptr, nullptr, 0)},
  };
  th_member reordered[] = {methods[1], methods[0]};
  th_type *trait = th_trait(types, S("base.Readable"), methods, 2, nullptr,
                            0);
  th_type *other_order = th_trait(types, S("base.Readable"), reordered, 2,
      nullptr, 0);
  th_type *other_name = th_trait(types, S("base.Writable"), methods, 2,
      nullptr, 0);
  th_type *clone = th_type_clone(copy_types, trait);
  uint64_t hash = th_hash(types, trait);
  const uint8_t *encoded = nullptr;
  size_t encoded_size = 0;
  static const char expected_encoding[] =
      "0b000000000000000d626173652e5265616461626c6500000000000000020000"
      "00000000000472656164060401000000000000000003696e7400000000000000"
      "000000000000000001000a0000000000000000010000000000000000036e696c"
      "0000000000000000000000000000000572656164790604010000000000000000"
      "04626f6f6c00000000000000000000000000000001000a000000000000000001"
      "0000000000000000036e696c00000000000000000000000000000000";
  bool vector = th_encode(types, trait, &encoded, &encoded_size) &&
      encoded_size * 2 == sizeof(expected_encoding) - 1;
  for (size_t i = 0; vector && i < encoded_size; i++) {
    char pair[3];
    snprintf(pair, sizeof(pair), "%02x", encoded[i]);
    vector = pair[0] == expected_encoding[2 * i] &&
        pair[1] == expected_encoding[2 * i + 1];
  }
  if (!vector) {
    fprintf(stderr, "trait hash: 0x%016llx\ntrait encoding: ",
        (unsigned long long)hash);
    for (size_t i = 0; i < encoded_size; i++)
      fprintf(stderr, "%02x", encoded[i]);
    fputc('\n', stderr);
  }
  check("trait canonical bytes and hash are stable",
        vector && hash == UINT64_C(0x7f02b0a21c34459f));
  core_string_t name = {};
  check("trait node exposes its ordered interface",
        th_trait_name(trait, &name) &&
        core_string_eq(name, S("base.Readable")) && th_trait_len(trait) == 2 &&
        th_trait_method(trait, 0) &&
        core_string_eq(th_trait_method(trait, 0)->name, S("read")) &&
        th_trait_method(trait, 1) &&
        core_string_eq(th_trait_method(trait, 1)->name, S("ready")));
  check("trait identity includes ordered methods and qualified name",
        trait && other_order && other_name && hash &&
        hash != th_hash(types, other_order) &&
        hash != th_hash(types, other_name) &&
        hash != th_hash(types, th_name(types, S("base.Readable"))) &&
        clone && hash == th_hash(copy_types, clone));
  th_member duplicate[] = {methods[0], methods[0]};
  th_member malformed = {.name = S("bad.method"), .type = methods[0].type};
  check("trait builder rejects duplicate and malformed methods",
        !th_trait(types, S("base.Readable"), duplicate, 2, nullptr, 0) &&
        !th_trait(types, S("base.Readable"), &malformed, 1, nullptr, 0));
  check("trait method order participates in unification",
        !th_unify(types, trait, other_order));

  th_type *string = th_name(types, S("string"));
  th_trait_requirement requirements[] = {
      {.subject = receiver, .kind = TH_CON_EXCLUDES, .target = string},
      {.subject = receiver, .kind = TH_CON_IMPLEMENTS,
       .iface = S("base.Copy")},
  };
  th_trait_requirement reverse_requirements[] = {
      requirements[1], requirements[0],
  };
  th_trait_requirement repeated_requirements[] = {
      requirements[0], requirements[1], requirements[0],
  };
  th_trait_requirement other_requirement = {
      .subject = receiver, .kind = TH_CON_EXCLUDES, .target = boolean,
  };
  th_type *constrained = th_trait(types, S("base.Readable"), methods, 2,
      requirements, 2);
  th_type *reordered_constraints = th_trait(types, S("base.Readable"),
      methods, 2, reverse_requirements, 2);
  th_type *repeated_constraints = th_trait(types, S("base.Readable"),
      methods, 2, repeated_requirements, 3);
  th_type *different_constraint = th_trait(types, S("base.Readable"),
      methods, 2, &other_requirement, 1);
  uint64_t constrained_hash = th_hash(types, constrained);
  const uint8_t *constrained_encoding = nullptr;
  size_t constrained_encoding_size = 0;
  bool constrained_encoded = th_encode(types, constrained,
      &constrained_encoding, &constrained_encoding_size);
  static const char expected_constrained_encoding[] =
      "0b000000000000000d626173652e5265616461626c6500000000000000020000"
      "00000000000472656164060401000000000000000003696e7400000000000000"
      "000000000000000001000a0000000000000000010000000000000000036e696c"
      "0000000000000000000000000000000572656164790604010000000000000000"
      "04626f6f6c00000000000000000000000000000001000a000000000000000001"
      "0000000000000000036e696c00000000000000000000000000000002020a0000"
      "0000000000000000000000000009626173652e436f7079060a00000000000000"
      "0001000000000000000006737472696e670000000000000000";
  size_t expected_constrained_length =
      sizeof(expected_constrained_encoding) - 1;
  bool constraint_vector = constrained_encoded &&
      constrained_encoding_size * 2 == expected_constrained_length;
  for (size_t i = 0; i < constrained_encoding_size &&
       2 * i + 1 < expected_constrained_length;
       i++) {
    char pair[3];
    snprintf(pair, sizeof(pair), "%02x", constrained_encoding[i]);
    bool same = pair[0] == expected_constrained_encoding[2 * i] &&
        pair[1] == expected_constrained_encoding[2 * i + 1];
    if (!same) {
      fprintf(stderr, "constrained vector differs at byte %zu: %.*s\n",
          i, 2, pair);
      constraint_vector = false;
      break;
    }
  }
  if (!constraint_vector)
    fprintf(stderr, "constrained hash: 0x%016llx; vector lengths: actual %zu expected %zu\n",
        (unsigned long long)constrained_hash,
        constrained_encoding_size * 2,
        expected_constrained_length);
  if (!constraint_vector) {
    for (size_t i = 0; i < constrained_encoding_size; i++)
      fprintf(stderr, "%02x", constrained_encoding[i]);
    fputc('\n', stderr);
  }
  check("constrained trait canonical bytes and hash are stable",
        constraint_vector &&
        constrained_hash == UINT64_C(0x2bdf055d88ee3f65));
  th_trait_requirement saved_requirement = {0};
  check("trait declared constraints affect identity without source-order drift",
        constrained && reordered_constraints && repeated_constraints &&
        different_constraint &&
        constrained_hash && constrained_hash != hash &&
        constrained_hash == th_hash(types, reordered_constraints) &&
        constrained_hash == th_hash(types, repeated_constraints) &&
        constrained_hash != th_hash(types, different_constraint) &&
        th_trait_requirement_count(constrained) == 2 &&
        th_trait_requirement_count(repeated_constraints) == 2 &&
        th_trait_requirement_at(constrained, 0, &saved_requirement) &&
        saved_requirement.subject == receiver);
  check("equivalent trait constraints unify regardless of order",
        th_unify(types, constrained, reordered_constraints) &&
        th_unify(types, constrained, repeated_constraints));

  th_type *other_variable = th_var(types);
  th_param pair[] = {
      {.mode = TH_IN, .type = receiver},
      {.mode = TH_IN, .type = other_variable},
  };
  th_member pair_method = {.name = S("pair"), .type =
      th_func_with_error(types, boolean, nil, pair, 2, nullptr, nullptr, 0)};
  th_trait_requirement on_first = {
      .subject = receiver, .kind = TH_CON_EXCLUDES, .target = string,
  };
  th_trait_requirement on_second = {
      .subject = other_variable, .kind = TH_CON_EXCLUDES, .target = string,
  };
  th_type *first_constrained = th_trait(types, S("base.Pair"), &pair_method,
      1, &on_first, 1);
  th_type *second_constrained = th_trait(types, S("base.Pair"), &pair_method,
      1, &on_second, 1);
  check("trait constraint subject participates in identity",
        first_constrained && second_constrained &&
        th_hash(types, first_constrained) !=
            th_hash(types, second_constrained));

  th_env *environment = th_env_new();
  check("register trait type graph for archive roundtrip",
        th_env_register_trait_contracts(environment, S("base.Readable"),
            S("base"), S("base"), TH_ENV_PUBLIC, trait) &&
        th_env_register_value(environment, S("base.trait_type"),
                              S("trait_type"), S("base"), S("base"),
                              TH_ENV_PUBLIC, trait) &&
        th_env_register_value(environment, S("base.constrained_type"),
                              S("constrained_type"), S("base"), S("base"),
                              TH_ENV_PUBLIC, constrained) &&
        th_env_freeze(environment));
  char path[] = "/tmp/lantern-trait-type-XXXXXX";
  core_string_t path_text = core_string_borrow(path, sizeof(path) - 1);
  int fd = mkstemp(path);
  if (fd >= 0) { close(fd); unlink(path); }
  core_object_archive_error_t error;
  bool saved = fd >= 0 && th_env_save(environment, path_text, 0, &error);
  th_env *loaded = saved ? th_env_load(path_text, &error) : nullptr;
  bool restored = false;
  bool restored_contract = false;
  bool restored_constraints = false;
  for (size_t i = 0; loaded && i < th_env_declaration_count(loaded); i++) {
    th_env_declaration_view_t view = {0};
    if (!th_env_declaration_at(loaded, i, &view) ||
        !view.qualified_name.data) continue;
    if (core_string_eq(view.qualified_name, S("base.trait_type")))
      restored = view.value_type &&
          th_kind_of(view.value_type) == TH_TRAIT &&
          th_hash(types, view.value_type) == hash;
    if (core_string_eq(view.qualified_name, S("base.Readable")))
      restored_contract = view.kind == TH_ENV_DECLARATION_TRAIT &&
          view.contract && th_kind_of(view.contract) == TH_TRAIT &&
          th_trait_len(view.contract) == 2 &&
          th_hash(types, view.contract) == hash;
    if (core_string_eq(view.qualified_name, S("base.constrained_type")))
      restored_constraints = view.value_type &&
          th_kind_of(view.value_type) == TH_TRAIT &&
          th_trait_requirement_count(view.value_type) == 2 &&
          th_hash(types, view.value_type) == constrained_hash;
  }
  check("trait type and contract survive environment archive",
        restored && restored_contract && restored_constraints);
  th_env_free(loaded);
  if (fd >= 0) unlink(path);
  th_env_free(environment);
  th_ctx_free(copy_types);
  th_ctx_free(types);
}

static void test_consume_ref_method(void) {
  th_ctx *types = th_ctx_new();
  th_env *environment = th_env_new();
  th_type *nil = th_name(types, S("nil"));
  th_type *variable = th_var(types);
  th_param required = {.mode = TH_CONSUME, .type = variable};
  th_type *trait_method = th_func_with_error(types, nil, nil, &required, 1,
                                             nullptr, nullptr, 0);
  th_type *sample = th_name(types, S("example.Sample"));
  th_type *reference = th_ind(types, 0, sample);
  th_param provided = {.mode = TH_CONSUME, .type = reference};
  th_type *implementation = th_func_with_error(types, nil, nil, &provided,
                                                1, nullptr, nullptr, 0);
  check("register consume reference trait method",
        trait_method && implementation &&
        th_env_register_type(environment, S("example.Sample"), S("example"),
                             S("base"), TH_ENV_PUBLIC) &&
        th_env_register_trait_contracts(environment, S("example.Probe"),
            S("example"), S("base"), TH_ENV_PRIVATE,
            method_record(types, S("apply"), trait_method)) &&
        th_env_register_function_contract(environment, S("example.apply"),
            S("apply"), S("example"), S("base"), TH_ENV_PRIVATE,
            th_env_signature_contract(types, implementation, nil)) &&
        th_env_freeze(environment) &&
        th_ctx_set_environment(types, environment, S("example"), S("base")));
  th_env_method_t method = {0};
  check("consume reference trait method resolves",
        th_env_method_resolve(types, reference, S("example.Probe"),
                              S("apply"), &method) == TH_ENV_METHOD_FOUND &&
        core_string_eq(method.qualified_name, S("example.apply")));
  core_string_t trait_name = {};
  check("consume reference trait method resolves by name",
        th_env_method_resolve_by_name(types, reference, S("apply"), &method,
                                      &trait_name) == TH_ENV_METHOD_FOUND &&
        trait_name.data && core_string_eq(trait_name, S("example.Probe")));
  check("private consume reference method hidden outside its module",
        th_ctx_set_environment_scope(types, S("other"), S("base")) &&
        th_env_method_resolve_by_name(types, reference, S("apply"), &method,
                                      &trait_name) == TH_ENV_METHOD_NOT_FOUND);
  th_env_free(environment);
  th_ctx_free(types);
}

static void test_array_option_method(void) {
  th_ctx *types = th_ctx_new();
  th_env *environment = th_env_new();
  th_type *required_receiver = th_var(types);
  th_type *required_index = th_var(types);
  th_type *required_result = th_var(types);
  th_param required[] = {
      {.mode = TH_IN, .type = required_receiver},
      {.mode = TH_IN, .type = required_index},
  };
  th_type *requirement = th_func_with_error(types, required_result,
      th_name(types, S("nil")), required, 2, nullptr, nullptr, 0);
  th_type *element = th_var(types);
  th_param provided[] = {
      {.mode = TH_IN, .type = th_array(types, element)},
      {.mode = TH_IN, .type = th_name(types, S("int"))},
  };
  th_type *implementation = th_func_with_error(types, element,
      th_name(types, S("nil")), provided, 2, nullptr, nullptr, 0);
  check("register generic array read method",
      requirement && implementation &&
      th_env_register_trait_contracts(environment, S("Index_Read"),
          S("builtins"), S("base"), TH_ENV_PUBLIC,
          method_record(types, S("__read"), requirement)) &&
      th_env_register_function_contract(environment,
          S("builtins.__array_read"), S("__read"), S("builtins"), S("base"),
          TH_ENV_PUBLIC,
          th_env_signature_contract(types, implementation,
              th_name(types, S("nil")))) &&
      th_env_freeze(environment));
  th_ctx *use = th_ctx_new();
  th_type *reference = th_ind(use, 0, th_name(use, S("byte")));
  th_arg item = {.kind = TH_ARG_TYPE, .type = reference};
  th_type *optional = th_apply(use, S(TH_OPTION), &item, 1);
  th_type *array = th_array(use, optional);
  check("generic read resolves for array of optional references",
      th_ctx_set_environment(use, environment, S("client"), S("base")) &&
      th_env_method_resolve_by_name(use, array, S("__read"), nullptr,
                                    nullptr) == TH_ENV_METHOD_FOUND);
  th_ctx *late = th_ctx_new();
  th_ctx_set_scope(late, S("client"), S("base"));
  th_type *late_receiver = th_var(late);
  th_type *late_result = th_var(late);
  th_type *late_reference = th_ind(late, 0, th_name(late, S("byte")));
  th_arg late_item = {.kind = TH_ARG_TYPE, .type = late_reference};
  th_type *late_optional = th_apply(late, S(TH_OPTION), &late_item, 1);
  th_param index = {.mode = TH_IN, .type = th_name(late, S("int"))};
  th_type *call = th_func_with_error(late, late_result,
      th_name(late, S("nil")), &index, 1, nullptr, nullptr, 0);
  bool obligation_added = th_require_method(late, late_receiver, S("__read"),
                                            late_result, call);
  bool bound = obligation_added && th_unify_in_place(late, late_receiver,
      th_array(late, late_optional)) != nullptr;
  check("deferred array read resolves optional reference result",
      bound && th_ctx_set_environment(late, environment, S("client"),
                                      S("base")) &&
      th_unify(late, late_result, late_optional));
  th_ctx_free(late);
  th_ctx_free(use);
  th_env_free(environment);
  th_ctx_free(types);
}

static void test_record_method_argument(void) {
  th_ctx *types = th_ctx_new();
  th_env *environment = th_env_new();
  th_param required[] = {
      {.mode = TH_INOUT, .type = th_var(types)},
      {.mode = TH_IN, .type = th_var(types)},
      {.mode = TH_IN, .type = th_var(types)},
  };
  th_type *requirement = th_func_with_error(types, th_var(types),
      th_var(types), required, 3, nullptr, nullptr, 0);
  th_type *element = th_var(types);
  th_param parameters[] = {
      {.mode = TH_INOUT, .type = th_array(types, element)},
      {.mode = TH_IN, .type = th_name(types, S("int"))},
      {.mode = TH_IN, .type = element},
  };
  th_type *signature = th_func_with_error(types, th_name(types, S("nil")),
      th_name(types, S("nil")), parameters, 3, nullptr, nullptr, 0);
  bool registered = requirement && signature &&
      th_env_register_trait_contracts(environment, S("Index_Write"),
          S("builtins"), S("base"), TH_ENV_PUBLIC,
          method_record(types, S("__write"), requirement)) &&
      th_env_register_function_contract(environment,
          S("builtins.array_write"), S("__write"), S("builtins"), S("base"),
          TH_ENV_PUBLIC,
          th_env_signature_contract(types, signature,
                                    th_name(types, S("nil")))) &&
      th_env_freeze(environment);
  check("register generic array write method", registered);

  for (size_t test = 0; registered && test < 4; test++) {
    th_ctx *use = th_ctx_new();
    th_type *pair = th_name(use, S("client.Pair"));
    th_member fields[] = {{.name = S("left"), .type = th_name(use, S("int"))},
                          {.name = S("right"),
                           .type = th_name(use, S("int"))}};
    th_type *definition = th_record(use, fields, 2);
    th_member wrong_fields[] = {
        {.name = S("left"), .type = th_name(use, S("int"))},
        {.name = S("other"), .type = th_name(use, S("int"))},
    };
    th_type *inferred_left = th_var(use);
    th_member inferred_fields[] = {
        {.name = S("left"), .type = inferred_left},
        {.name = S("right"), .type = th_name(use, S("int"))},
    };
    th_type *actual = test == 2 ? th_record(use, wrong_fields, 2)
                    : test == 3 ? th_record(use, inferred_fields, 2)
                                : th_record(use, fields, 2);
    th_param arguments[] = {
        {.mode = TH_IN, .type = th_name(use, S("int"))},
        {.mode = TH_IN, .type = actual},
    };
    th_type *result = th_var(use);
    th_type *call = th_func(use, result, arguments, 2, nullptr, nullptr, 0);
    th_type *receiver = th_var(use);
    bool ready = pair && definition && actual && call && receiver &&
        th_type_define(use, pair, definition) &&
        th_require_method(use, receiver, S("__write"), result, call);
    if (test != 0)
      th_ctx_set_method_argument_coercions(use, TH_COERCE_RECORD_CONSTRUCT);
    bool bound = ready && th_unify_in_place(use, receiver,
        th_array(use, pair)) &&
        th_ctx_set_environment(use, environment, S("client"), S("base"));
    check(test == 0 ? "record method argument rejects implicit construction by default"
                    : test == 1 ? "record method argument accepts matching nominal shape"
                    : test == 2 ? "record method argument rejects mismatched nominal shape"
                                : "record method argument binds matching field types",
          bound == (test == 1 || test == 3) &&
          (test != 3 || th_kind_of(th_resolve(inferred_left)) == TH_NAMED));
    th_ctx_free(use);
  }
  th_env_free(environment);
  th_ctx_free(types);
}

int main(void) {
  test_consume_ref_method();
  test_array_option_method();
  test_record_method_argument();
  test_kind_constraint_archive();
  test_trait_type_identity();
  th_env *env = th_env_new();
  th_ctx *types = th_ctx_new();
  th_type *integer = th_name(types, S("int"));
  th_type *string = th_name(types, S("string"));

  check("register type origin",
        th_env_register_type(env, S("int"), S("ints"), S("base"),
                             TH_ENV_PUBLIC));
  check("type origin visibility is part of its stable declaration",
        !th_env_register_type(env, S("int"), S("ints"), S("base"),
                              TH_ENV_PRIVATE) &&
            th_env_register_type(env, S("Secret"), S("secrets"), S("base"),
                                 TH_ENV_PRIVATE) &&
            th_env_register_type(env, S("PackageType"), S("internal"),
                                 S("base"), TH_ENV_PACKAGE));
  th_ctx *utility_types = th_ctx_new();
  check("ordinary function signature is borrowed from its module context",
        register_function(env, S("base.utility"), S("utility"), S("utility"),
                          S("base"), TH_ENV_PUBLIC,
                          fn0(utility_types,
                              th_name(utility_types, S("int")))));
  check("duplicate qualified function identities are rejected",
        !register_function(
            env, S("base.utility"), S("utility"), S("utility"), S("base"),
            TH_ENV_PUBLIC,
            fn0(utility_types, th_name(utility_types, S("string")))));

  th_type *read_receiver = th_var(types);
  check("register trait with unbound receiver parameter",
        th_env_register_trait(
            env, S("Readable"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("read"),
                          fn1(types, read_receiver, integer))));
  check("reject trait with concrete receiver parameter",
        !th_env_register_trait(
            env, S("Bad"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("bad"), fn1(types, integer, integer))));

  th_type *read_int = fn1(types, integer, integer);

  th_type *nested_add_receiver = th_var(types);
  th_type *nested_add_argument = th_var(types);
  th_type *nested_add_result = th_var(types);
  check("register nested-result operator contract",
        th_env_register_trait(env, S("NestedAdd"), S("traits"), S("base"),
            TH_ENV_PUBLIC, method_record(types, S("__add_nested"),
                fn2(types, nested_add_receiver, nested_add_argument,
                    nested_add_result))) &&
        register_function(env, S("base.ints.add_nested"), S("__add_nested"),
            S("ints"), S("base"), TH_ENV_PUBLIC,
            fn2(types, integer, integer, integer)));
  th_type *nested_probe_receiver = th_var(types);
  check("register nested-result producer contract",
        th_env_register_trait(env, S("NestedProbe"), S("traits"), S("base"),
            TH_ENV_PUBLIC, method_record(types, S("probe_nested"),
                fn1(types, nested_probe_receiver, integer))) &&
        register_function(env, S("base.ints.probe_nested"),
            S("probe_nested"), S("ints"), S("base"), TH_ENV_PUBLIC,
            fn1(types, integer, integer)));
  check("register first package method candidate",
        register_function(env, S("base.ints.read_first"), S("read"),
                          S("ints"), S("base"), TH_ENV_PACKAGE, read_int));
  check("register same-rank method candidate in import order",
        register_function(env, S("base.ints.read_second"), S("read"),
                          S("ints"), S("base"), TH_ENV_PACKAGE, read_int));
  check("register package fallback candidate",
        register_function(env, S("base.other.read"), S("read"), S("other"),
                          S("base"), TH_ENV_PACKAGE, read_int));

  th_type *paired_receiver = th_var(types);
  th_member paired_requirements[] = {
      {.name = S("first"),
       .type = fn2(types, paired_receiver, paired_receiver, integer)},
      {.name = S("second"),
       .type = fn2(types, paired_receiver, paired_receiver, integer)},
  };
  check("register trait with shared receiver variable",
        th_env_register_trait(env, S("Paired"), S("traits"), S("base"),
                              TH_ENV_PUBLIC,
                              th_record(types, paired_requirements, 2)));
  check("register first matching function for paired trait",
        register_function(env, S("base.impl.first"), S("first"), S("impl"),
                          S("base"), TH_ENV_PUBLIC,
                          fn2(types, integer, integer, integer)));
  check("register mismatching second function for paired trait",
        register_function(env, S("base.impl.second"), S("second"), S("impl"),
                          S("base"), TH_ENV_PUBLIC,
                          fn2(types, integer, string, integer)));

  th_type *trait_element = th_var(types);
  check("register generic Box trait",
        th_env_register_trait(
            env, S("BoxReadable"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("get"),
                          fn1(types, trait_element, trait_element))));

  th_type *generic_element = th_var(types);
  th_arg generic_box_arg = {.kind = TH_ARG_TYPE, .type = generic_element};
  th_type *generic_box = th_apply(types, S("Box"), &generic_box_arg, 1);
  check("register generic Box function candidate",
        register_function(env, S("base.box.generic_get"), S("get"),
                          S("box-generic"), S("base"), TH_ENV_PACKAGE,
                          fn1(types, generic_box, generic_box)));

  th_arg concrete_box_arg = {.kind = TH_ARG_TYPE, .type = integer};
  th_type *concrete_box = th_apply(types, S("Box"), &concrete_box_arg, 1);
  check("register concrete Box function candidate",
        register_function(env, S("base.box.int_get"), S("get"), S("box-int"),
                          S("base"), TH_ENV_PACKAGE,
                          fn1(types, concrete_box, concrete_box)));

  th_type *pick_generic_receiver = th_var(types);
  th_type *pick_specific_receiver = th_var(types);
  th_type *pick_incomplete_receiver = th_var(types);
  th_member incomplete_pick_methods[] = {
      {.name = S("pick"),
       .type = fn1(types, pick_incomplete_receiver, string)},
      {.name = S("missing"),
       .type = fn1(types, pick_incomplete_receiver, string)},
  };
  th_arg pick_generic_arg = {.kind = TH_ARG_TYPE, .type = th_var(types)};
  th_type *pick_generic_box = th_apply(types, S("Box"), &pick_generic_arg, 1);
  check("register overlapping methods in distinct traits",
        th_env_register_trait(env, S("GenericPick"), S("traits"), S("base"),
            TH_ENV_PUBLIC, method_record(types, S("pick"),
                fn1(types, pick_generic_receiver, integer))) &&
        th_env_register_trait(env, S("SpecificPick"), S("traits"), S("base"),
            TH_ENV_PUBLIC, method_record(types, S("pick"),
                fn1(types, pick_specific_receiver, string))) &&
        th_env_register_trait(env, S("IncompletePick"), S("traits"),
            S("base"), TH_ENV_PUBLIC,
            th_record(types, incomplete_pick_methods, 2)) &&
        register_function(env, S("base.box.generic_pick"), S("pick"),
            S("box-generic"), S("base"), TH_ENV_PUBLIC,
            fn1(types, pick_generic_box, integer)) &&
        register_function(env, S("base.box.specific_pick"), S("pick"),
            S("box-int"), S("base"), TH_ENV_PUBLIC,
            fn1(types, concrete_box, string)));

  th_type *echo_receiver = th_var(types);
  check("register a self-returning method trait",
        th_env_register_trait(
            env, S("SelfEcho"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("echo"),
                          fn1(types, echo_receiver, echo_receiver))));
  check("register a concrete self-returning method",
        register_function(env, S("base.box.self_echo"), S("echo"),
                          S("box-int"), S("base"), TH_ENV_PACKAGE,
                          fn1(types, concrete_box, concrete_box)));

  th_type *returns_argument_receiver = th_var(types);
  th_type *returns_argument_value = th_var(types);
  check("register trait with an independent generic argument and result",
        th_env_register_trait(
            env, S("ReturnsArgument"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("echo"),
                fn2(types, returns_argument_receiver, returns_argument_value,
                    returns_argument_value))));
  th_type *echo_value = th_var(types);
  check("register generic method implementation",
        register_function(env, S("base.ints.echo"), S("echo"), S("ints"),
                          S("base"), TH_ENV_PACKAGE,
                          fn2(types, integer, echo_value, echo_value)));

  th_type *pair_trait_receiver = th_var(types);
  check("register trait used to test overlapping receiver patterns",
        th_env_register_trait(
            env, S("PairReadable"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("get"),
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
            env, S("base.pairs.same_get"), S("get"), S("pairs"), S("base"),
            TH_ENV_PUBLIC,
            fn1(types, th_apply(types, S("Pair"), same_pair_args, 2),
                integer)));
  check("register partially concrete pair method",
        register_function(
            env, S("base.pairs.int_get"), S("get"), S("pairs"), S("base"),
            TH_ENV_PUBLIC,
            fn1(types, th_apply(types, S("Pair"), first_int_pair_args, 2),
                integer)));

  th_type *map_trait_receiver = th_var(types);
  check("register trait for overlapping map receiver patterns",
        th_env_register_trait(
            env, S("MapReadable"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("map_get"),
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
        register_function(env, S("base.maps.int_get"), S("map_get"),
                          S("maps-int"), S("base"), TH_ENV_PUBLIC,
                          fn1(types, th_apply(types, S("Map"),
                                              int_value_map_args, 2),
                              integer)));
  check("register second incomparable map method",
        register_function(env, S("base.maps.text_get"), S("map_get"),
                          S("maps-text"), S("base"), TH_ENV_PUBLIC,
                          fn1(types, th_apply(types, S("Map"),
                                              text_value_map_args, 2),
                              integer)));

  th_type *integer_text_receiver = th_var(types);
  check("register integer text trait",
        th_env_register_trait(
            env, S("IntegerText"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("show"),
                          fn1(types, integer_text_receiver, string))));
  check("register wider signed-integer dispatch candidates",
        register_function(env, S("base.int16.show"), S("show"), S("int16"),
                          S("base"), TH_ENV_PUBLIC,
                          fn1(types, th_bitint(types, 16, true), string)) &&
            register_function(env, S("base.int32.show"), S("show"),
                              S("int32"), S("base"), TH_ENV_PUBLIC,
                              fn1(types, th_bitint(types, 32, true), string)));

  th_type *index_receiver = th_var(types);
  check("register built-in read-index trait contract",
        th_env_register_trait(
            env, S("Index_Load"), S("builtins"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("load"),
                          fn2(types, index_receiver, integer, string))));
  th_param store_params[] = {
      {.mode = TH_IN, .type = th_var(types)},
      {.mode = TH_IN, .type = integer},
      {.mode = TH_IN, .type = string},
  };
  check("register built-in write-index trait contract",
        th_env_register_trait(
            env, S("Index_Store"), S("builtins"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("store"),
                          th_func(types, integer, store_params, 3, nullptr,
                                  nullptr, 0))));
  th_type *int_array = th_array(types, integer);
  check("register ordinary read/write index implementations",
        register_function(env, S("base.array.load"), S("load"), S("array"),
                          S("base"), TH_ENV_PUBLIC,
                          fn2(types, int_array, integer, string)) &&
            register_function(env, S("base.array.store"), S("store"),
                              S("array"), S("base"), TH_ENV_PUBLIC,
                              th_func(types, integer,
                                      (th_param[]){{TH_IN, int_array},
                                                   {TH_IN, integer},
                                                   {TH_IN, string}},
                                      3, nullptr, nullptr, 0)));

  th_type *add_receiver = th_var(types);
  check("register overloaded addition trait contract",
        th_env_register_trait(
            env, S("Add"), S("builtins"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("add"),
                          fn2(types, add_receiver, add_receiver,
                              add_receiver))));
  check("register ordinary integer addition function",
        register_function(env, S("base.int.add"), S("add"), S("int"),
                          S("base"), TH_ENV_PUBLIC,
                          fn2(types, integer, integer, integer)));

  th_type *fallible_receiver = th_var(types);
  th_type *read_error = th_name(types, S("ReadError"));
  check("register trait success and failure channels",
        th_env_register_trait_channels(
            env, S("FallibleRead"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(types, S("fallible_read"),
                          fn1(types, fallible_receiver, string)),
            method_record(types, S("fallible_read"), read_error)) &&
            th_env_register_function_channels(
                env, S("base.reads.int_read"), S("fallible_read"),
                S("reads"), S("base"), TH_ENV_PUBLIC,
                fn1(types, integer, string), read_error) &&
            th_env_register_function_channels(
                env, S("base.reads.wrong_error"), S("fallible_read"),
                S("reads"), S("base"), TH_ENV_PUBLIC,
                fn1(types, integer, string),
                th_name(types, S("WriteError"))));

  th_type *correlated_channel_var = th_var(types);
  th_type *correlated_contract = th_generalize(types,
      th_env_signature_contract(types,
          fn1(types, correlated_channel_var, correlated_channel_var),
          correlated_channel_var));
  th_type *wrong_correlated_contract = th_env_signature_contract(
      types, fn1(types, integer, integer), string);
  check("register jointly quantified trait and function channel contracts",
        th_env_register_trait_contracts(
            env, S("CorrelatedFailure"), S("traits"), S("base"),
            TH_ENV_PUBLIC,
            method_record(types, S("correlated"), correlated_contract)) &&
            th_env_register_function_contract(
                env, S("base.reads.correlated_wrong"), S("correlated"),
                S("reads"), S("base"), TH_ENV_PUBLIC,
                wrong_correlated_contract));

  th_type *iter_source = th_var(types);
  th_type *iter_cursor = th_var(types);
  th_type *iter_error = th_var(types);
  th_param iter_receiver = {.mode = TH_IN, .type = iter_source};
  th_type *iter_signature = th_func_with_error(
      types, iter_cursor, iter_error, &iter_receiver, 1, nullptr, nullptr, 0);
  th_member iter_method = {
      .name = S("iter"),
      .type = th_generalize(types, iter_signature),
  };
  th_type *iter_contracts = th_record(types, &iter_method, 1);
  th_arg optional_int_arg = {.kind = TH_ARG_TYPE, .type = integer};
  th_type *optional_int = th_apply(types, S(TH_OPTION), &optional_int_arg,
                                   1);
  th_param iter_impl_receiver = {.mode = TH_IN, .type = optional_int};
  th_type *iter_impl_signature = th_func_with_error(
      types, optional_int, th_name(types, S("nil")), &iter_impl_receiver, 1,
      nullptr, nullptr, 0);
  th_type *iter_impl_contract = th_env_signature_contract(
      types, iter_impl_signature, th_name(types, S("nil")));
  check("register iterable trait with an independent cursor and failure type",
        iter_contracts && th_env_register_trait_contracts(
            env, S("IterableTest"), S("builtins"), S("base"), TH_ENV_PUBLIC,
            iter_contracts));
  check("register a concrete iterable method candidate",
        iter_impl_contract && th_env_register_function_contract(
            env, S("base.iterables.option_int"), S("iter"), S("iterables"),
            S("base"), TH_ENV_PUBLIC, iter_impl_contract));

  th_type *private_get_receiver = th_var(types);
  check("register private trait method for requirement visibility",
        th_env_register_trait(
            env, S("SecretGetter"), S("private_traits"), S("base"),
            TH_ENV_PRIVATE,
            method_record(types, S("get"),
                          fn1(types, private_get_receiver, integer))));

  check("freeze environment", th_env_freeze(env));

  th_ctx *inside = th_ctx_new();
  th_ctx *outside = th_ctx_new();
  check("attach contexts to frozen environment",
        th_ctx_set_environment(inside, env, S("consumer"), S("base")) &&
            th_ctx_set_environment(inside, env, S("consumer"), S("base")) &&
            th_ctx_set_environment(outside, env, S("consumer"), S("other")) &&
            !th_ctx_set_environment(inside, env, S("other"), S("base")));
  th_env_method_t nested_add = {0};
  check("primitive nested-result operator resolves directly",
        th_env_method_resolve(inside, integer, S("NestedAdd"),
                              S("__add_nested"),
                              &nested_add) == TH_ENV_METHOD_FOUND);
  th_ctx *nested_context = th_ctx_new();
  th_type *nested_return = th_var(nested_context);
  th_type *nested_right = th_var(nested_context);
  th_type *nested_sum = th_var(nested_context);
  th_type *nested_integer = th_name(nested_context, S("int"));
  bool nested_obligations = th_require_method(nested_context, nested_return,
      S("__add_nested"), nested_sum,
      fn1(nested_context, nested_right, nested_sum)) &&
      th_require_method(nested_context, nested_integer, S("probe_nested"),
          nested_return, fn0(nested_context, nested_return)) &&
      th_require_method(nested_context, nested_integer, S("probe_nested"),
          nested_right, fn0(nested_context, nested_right));
  check("nested method-result constraints resolve on environment attach",
        nested_obligations &&
        th_ctx_set_environment(nested_context, env, S("consumer"),
                               S("base")) &&
        th_unify(nested_context, nested_return, nested_integer) &&
        th_unify(nested_context, nested_right, nested_integer) &&
        th_unify(nested_context, nested_sum, nested_integer));
  th_ctx_free(nested_context);
  th_env_method_t first_echo = {0}, second_echo = {0};
  const th_param *first_echo_arg = nullptr, *second_echo_arg = nullptr;
  bool independent_echo_signatures =
      th_env_method_resolve(inside, integer, S("ReturnsArgument"), S("echo"),
                            &first_echo) == TH_ENV_METHOD_FOUND &&
      th_env_method_resolve(inside, integer, S("ReturnsArgument"), S("echo"),
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
        th_ctx_set_environment(secret_context, env, S("secrets"),
                               S("base")) &&
            th_env_type_visible(secret_context, S("Secret")));
  check("type visibility follows private and package scopes",
        th_env_type_visible(inside, S("int")) &&
            !th_env_type_visible(inside, S("Secret")) &&
            th_env_type_visible(inside, S("PackageType")) &&
            !th_env_type_visible(outside, S("PackageType")));
  check("shared type context can change module visibility view",
        th_ctx_set_environment_scope(outside, S("secrets"), S("base")) &&
            th_env_type_visible(outside, S("Secret")) &&
            th_ctx_set_environment_scope(outside, S("consumer"),
                                         S("other")) &&
            !th_env_type_visible(outside, S("Secret")));

  th_type *consumer_int = th_name(inside, S("int"));
  th_env_method_t selected = {0};
  th_arg optional_consumer_int_arg = {
      .kind = TH_ARG_TYPE,
      .type = consumer_int,
  };
  th_type *optional_consumer_int =
      th_apply(inside, S(TH_OPTION), &optional_consumer_int_arg, 1);
  th_env_method_t iterator_method = {0};
  check("independent trait result and generic failure channels resolve",
        th_env_method_candidates(inside, S("IterableTest"), S("iter"),
                                 nullptr, 0) == 1 &&
            th_env_method_lookup(inside, optional_consumer_int,
                                 S("IterableTest"), S("iter"),
                                 &iterator_method) &&
            core_string_eq(iterator_method.qualified_name,
                           S("base.iterables.option_int")));
  th_type *readable_variable = th_var(inside);
  check("record generic trait obligation on an unresolved variable",
        th_var_constrain(
            inside, readable_variable,
            (th_constraint){.kind = TH_CON_IMPLEMENTS,
                            .iface = S("Readable")}));
  core_string_t variable_traits[2] = {};
  check("unresolved trait obligations remain visible to inference",
        th_implements(inside, readable_variable, S("Readable")) &&
            th_infer_ifaces(inside, readable_variable, variable_traits, 2) ==
                1 &&
            core_string_eq(variable_traits[0], S("Readable")));
  check("unresolved trait obligations do not trigger concrete dispatch",
        th_env_method_resolve(inside, readable_variable, S("Readable"),
                              S("read"), &selected) ==
            TH_ENV_METHOD_NOT_FOUND);
  check("trait obligation accepts a matching concrete type",
        th_unify(inside, readable_variable, consumer_int) != nullptr);
  th_type *invalid_readable_variable = th_var(inside);
  th_var_constrain(
      inside, invalid_readable_variable,
      (th_constraint){.kind = TH_CON_IMPLEMENTS, .iface = S("Readable")});
  check("trait obligation rejects a concrete type without a matching method",
        th_unify(inside, invalid_readable_variable,
                 th_name(inside, S("string"))) == nullptr);
  check("ordinary function candidate supplies open-trait fact",
        th_implements(inside, consumer_int, S("Readable")));
  th_env_method_t fallible_candidates[2] = {0};
  th_env_method_t fallible_method = {0};
  check("trait candidate matching checks its separately tracked failure type",
        th_env_method_candidates(inside, S("FallibleRead"),
                                 S("fallible_read"),
                                 fallible_candidates, 2) == 1 &&
            core_string_eq(fallible_candidates[0].qualified_name,
                           S("base.reads.int_read")) &&
            th_env_method_lookup(inside, consumer_int, S("FallibleRead"),
                                 S("fallible_read"), &fallible_method) &&
            th_unify(inside, fallible_method.error_type, read_error) &&
            th_implements(inside, consumer_int, S("FallibleRead")));
  check("trait channel matching preserves shared quantified variables",
        !th_implements(inside, consumer_int, S("CorrelatedFailure")) &&
            th_env_method_candidates(inside, S("CorrelatedFailure"),
                                     S("correlated"), nullptr, 0) == 0);
  check("package-scoped candidate is invisible outside package",
        !th_implements(outside, th_name(outside, S("int")), S("Readable")));
  check("dispatch generation can enumerate viable method candidates",
        th_env_method_candidates(inside, S("Readable"), S("read"), nullptr,
                                 0) == 3 &&
            th_env_method_candidates(outside, S("Readable"), S("read"),
                                     nullptr, 0) == 0);
  th_env_method_requirement_t get_requirements[3] = {0};
  size_t get_requirement_count = th_env_method_requirements(
      inside, S("get"), get_requirements, 3);
  check("member inference can enumerate visible trait contracts by method",
        get_requirement_count == 2 &&
            core_string_eq(get_requirements[0].trait_name,
                           S("BoxReadable")) &&
            core_string_eq(get_requirements[1].trait_name,
                           S("PairReadable")) &&
            th_env_method_requirements(outside, S("get"), nullptr, 0) == 2);
  th_type *box_requirement_receiver = get_requirement_count
      ? (th_type *)th_func_param(get_requirements[0].signature, 0)->type
      : nullptr;
  th_type *box_requirement_result = get_requirement_count
      ? th_func_ret(get_requirements[0].signature) : nullptr;
  th_type *box_int = th_apply(
      inside, S("Box"),
      &(th_arg){.kind = TH_ARG_TYPE, .type = th_name(inside, S("int"))}, 1);
  check("method requirement keeps receiver and result variables related",
        box_requirement_receiver && box_requirement_result &&
            th_unify_in_place(inside, box_requirement_receiver, box_int) &&
            th_unify_in_place(inside, box_requirement_result, box_int));
  th_env_method_requirement_t repeated_requirements[2] = {0};
  size_t repeated_count = th_env_method_requirements(
      inside, S("get"), repeated_requirements, 2);
  th_type *repeated_receiver = repeated_count
      ? (th_type *)th_func_param(repeated_requirements[0].signature, 0)->type
      : nullptr;
  check("each method requirement query has independent type variables",
        repeated_count == 2 && repeated_receiver &&
            th_unify_in_place(inside, repeated_receiver,
                              th_name(inside, S("string"))));
  check("all methods are required for a trait fact",
        !th_implements(inside, consumer_int, S("Paired")));
  check("method dispatch requires the complete trait contract",
        !th_env_method_lookup(inside, consumer_int, S("Paired"), S("first"),
                              &selected));

  check("method lookup returns method candidate identity",
        th_env_method_lookup(inside, consumer_int, S("Readable"), S("read"),
                             &selected) &&
            core_string_eq(selected.qualified_name,
                           S("base.ints.read_first")));
  check("type module candidate outranks other package modules",
        th_env_method_lookup(inside, consumer_int, S("Readable"), S("read"),
                             &selected) &&
            core_string_eq(selected.module, S("ints")));

  check(
      "most specific function candidate wins dispatch",
      th_env_method_lookup(inside, box_int, S("BoxReadable"), S("get"),
                           &selected) &&
          core_string_eq(selected.qualified_name, S("base.box.int_get")));
  th_type *box_pattern_item = th_var(inside);
  th_arg box_pattern_arg = {
      .kind = TH_ARG_TYPE,
      .type = box_pattern_item,
  };
  th_type *box_pattern = th_apply(inside, S("Box"), &box_pattern_arg, 1);
  check("structural type-pattern subsumption distinguishes generic receivers",
        box_pattern && th_env_type_pattern_subsumes(box_pattern, box_int) &&
            !th_env_type_pattern_subsumes(box_int, box_pattern) &&
            th_env_type_pattern_subsumes(box_int, box_int));
  th_type *bit_pattern_item = th_var(inside);
  th_constraint bit_kind = {.kind = TH_CON_KIND, .type_kind = TH_BITINT};
  th_arg bit_pattern_arg = {.kind = TH_ARG_TYPE, .type = bit_pattern_item};
  th_type *bit_pattern = th_apply(inside, S("Box"), &bit_pattern_arg, 1);
  th_arg bit_arg = {.kind = TH_ARG_TYPE,
                    .type = th_bitint(inside, 32, true)};
  th_type *box_bit = th_apply(inside, S("Box"), &bit_arg, 1);
  check("concrete pattern matching enforces variable constraints",
        th_var_constrain(inside, bit_pattern_item, bit_kind) &&
            th_env_type_pattern_accepts(inside, bit_pattern, box_bit) &&
            !th_env_type_pattern_accepts(inside, bit_pattern, box_int) &&
            !th_env_type_pattern_accepts(inside, bit_pattern, box_pattern));
  core_string_t selected_trait = {};
  check("method-name lookup selects the applicable visible trait contract",
        th_env_method_resolve_by_name(inside, box_int, S("get"), &selected,
                                      &selected_trait) ==
                TH_ENV_METHOD_FOUND &&
            core_string_eq(selected.qualified_name, S("base.box.int_get")) &&
            core_string_eq(selected_trait, S("BoxReadable")));
  check("method-name lookup ranks receivers across complete traits",
        th_implements(inside, box_int, S("GenericPick")) &&
            th_implements(inside, box_int, S("SpecificPick")) &&
            !th_implements(inside, box_int, S("IncompletePick")) &&
            th_env_method_resolve_by_name(inside, box_int, S("pick"),
                                          &selected, &selected_trait) ==
                TH_ENV_METHOD_FOUND &&
            core_string_eq(selected.qualified_name,
                           S("base.box.specific_pick")) &&
            core_string_eq(selected_trait, S("SpecificPick")));
  th_type *unspecialized_item = th_var(inside);
  th_arg unspecialized_arg = {.kind = TH_ARG_TYPE,
                              .type = unspecialized_item};
  th_type *unspecialized_box =
      th_apply(inside, S("Box"), &unspecialized_arg, 1);
  check("exact method does not specialize a generic receiver",
        th_env_method_resolve_by_name(inside, unspecialized_box, S("pick"),
                                      &selected, &selected_trait) ==
                TH_ENV_METHOD_FOUND &&
            core_string_eq(selected.qualified_name,
                           S("base.box.generic_pick")) &&
            core_string_eq(selected_trait, S("GenericPick")) &&
            th_resolve(unspecialized_item) == unspecialized_item);
  th_env_vtable_slot_t box_slots[1] = {0};
  size_t box_slot_count = 0;
  th_env_vtable_plan_t box_vtable = {0};
  check("concrete trait values receive a complete vtable plan",
        th_env_trait_vtable_plan(inside, S("BoxReadable"), box_int,
                                 box_slots, 1, &box_slot_count,
                                 &box_vtable) == TH_ENV_METHOD_FOUND &&
            box_slot_count == 1 &&
            box_vtable.concrete_type == box_int &&
            box_vtable.concrete_type_hash == th_hash(inside, box_int) &&
            box_vtable.slot_count == 1 &&
            box_vtable.required_slot_count == 1 &&
            core_string_eq(box_slots[0].method_name, S("get")) &&
            core_string_eq(box_slots[0].implementation.qualified_name,
                           S("base.box.int_get")));
  th_env_vtable_slot_t missing_slots[2] = {
      {.method_name = S("unchanged")}, {.method_name = S("unchanged")}};
  size_t missing_slot_count = 7;
  check("incomplete trait implementations cannot produce partial vtables",
        th_env_trait_vtable_plan(inside, S("Paired"), consumer_int,
                                 missing_slots, 2, &missing_slot_count,
                                 nullptr) ==
                TH_ENV_METHOD_NOT_FOUND &&
            missing_slot_count == 0 &&
            core_string_eq(missing_slots[0].method_name, S("unchanged")) &&
            core_string_eq(missing_slots[1].method_name, S("unchanged")));
  th_arg concrete_pair_args[] = {
      {.kind = TH_ARG_TYPE, .type = th_name(inside, S("int"))},
      {.kind = TH_ARG_TYPE, .type = th_name(inside, S("int"))},
  };
  th_type *concrete_pair = th_apply(inside, S("Pair"), concrete_pair_args,
                                    2);
  th_type *scheme_receiver = th_var(inside);
  th_type *scheme_result = th_var(inside);
  check("retain a deferred method result on an unresolved receiver",
        th_var_constrain(
            inside, scheme_receiver,
            (th_constraint){.kind = TH_CON_METHOD,
                            .method = {.name = S("get"),
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
                                .name = S("echo"),
                                .result = self_echo_receiver,
                                .call = fn0(inside, self_echo_receiver),
                            }}) &&
            th_unify_in_place(inside, self_echo_receiver, box_int));
  th_type *open_echo_receiver = th_var(inside);
  th_constraint open_echo_obligation = {
      .kind = TH_CON_METHOD,
      .method = {
          .name = S("echo"),
          .result = open_echo_receiver,
          .call = fn1(inside, open_echo_receiver, open_echo_receiver),
      },
  };
  bool open_echo_constrained =
      th_var_constrain(inside, open_echo_receiver, open_echo_obligation) &&
      th_var_constrain(inside, open_echo_receiver, open_echo_obligation);
  th_type *open_echo_call = th_type_clone(
      inside, fn1(inside, open_echo_receiver, open_echo_receiver));
  const th_param *open_echo_parameter = open_echo_call
      ? th_func_param(open_echo_call, 0) : nullptr;
  check("equivalent method obligations occupy one constraint slot",
        open_echo_constrained &&
            th_var_constraint_count(open_echo_receiver) == 1);
  check("cloned open method graph resolves repeated receiver occurrences",
        open_echo_parameter &&
            th_unify_in_place(inside, open_echo_parameter->type,
                              consumer_int));
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
                           th_name(inside, S("string"))));
  th_ctx *late_environment_context = th_ctx_new();
  th_type *late_receiver = th_var(late_environment_context);
  th_type *late_result = th_var(late_environment_context);
  th_var_constrain(late_environment_context, late_receiver,
                   (th_constraint){.kind = TH_CON_METHOD,
                                   .method = {.name = S("get"),
                                              .result = late_result}});
  th_arg late_box_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(late_environment_context, S("int")),
  };
  th_type *late_box = th_apply(late_environment_context, S("Box"),
                               &late_box_arg, 1);
  bool bound_before_environment = th_unify_in_place(
      late_environment_context, late_receiver, late_box) != nullptr;
  check("environment attachment validates previously bound method obligations",
        bound_before_environment &&
            th_ctx_set_environment(late_environment_context, env,
                                   S("consumer"), S("base")) &&
            th_unify(late_environment_context, late_result,
                     late_box));
  th_ctx *late_invalid_context = th_ctx_new();
  th_type *late_invalid_receiver = th_var(late_invalid_context);
  th_var_constrain(late_invalid_context, late_invalid_receiver,
                   (th_constraint){.kind = TH_CON_METHOD,
                                   .method = {.name = S("get"),
                                              .result = th_var(late_invalid_context)}});
  bool invalid_bound_early = th_unify_in_place(
      late_invalid_context, late_invalid_receiver,
      th_name(late_invalid_context, S("string"))) != nullptr;
  check("late environment attachment reports a missing method implementation",
        invalid_bound_early &&
            !th_ctx_set_environment(late_invalid_context, env,
                                    S("consumer"), S("base")));
  th_ctx *late_trait_context = th_ctx_new();
  th_type *late_trait_receiver = th_var(late_trait_context);
  th_var_constrain(late_trait_context, late_trait_receiver,
                   (th_constraint){.kind = TH_CON_IMPLEMENTS,
                                   .iface = S("Readable")});
  bool trait_bound_before_environment = th_unify_in_place(
      late_trait_context, late_trait_receiver,
      th_name(late_trait_context, S("int"))) != nullptr;
  check("environment attachment validates a trait bound after early binding",
        trait_bound_before_environment &&
            th_ctx_set_environment(late_trait_context, env, S("consumer"),
                                   S("base")));
  th_ctx *late_invalid_trait_context = th_ctx_new();
  th_type *late_invalid_trait_receiver = th_var(late_invalid_trait_context);
  th_var_constrain(late_invalid_trait_context, late_invalid_trait_receiver,
                   (th_constraint){.kind = TH_CON_IMPLEMENTS,
                                   .iface = S("Readable")});
  bool invalid_trait_bound_early = th_unify_in_place(
      late_invalid_trait_context, late_invalid_trait_receiver,
      th_name(late_invalid_trait_context, S("string"))) != nullptr;
  check("environment attachment rejects a failed trait bound after binding",
        invalid_trait_bound_early &&
            !th_ctx_set_environment(late_invalid_trait_context, env,
                                    S("consumer"), S("base")));
  th_ctx *scoped_method_context = th_ctx_new();
  th_type *scoped_method_receiver = th_var(scoped_method_context);
  th_type *scoped_method_result = th_var(scoped_method_context);
  bool scoped_method_added = th_ctx_set_scope(
      scoped_method_context, S("consumer"), S("base")) &&
      th_var_constrain(scoped_method_context, scoped_method_receiver,
          (th_constraint){.kind = TH_CON_METHOD,
              .method = {.name = S("read"), .result = scoped_method_result}});
  bool scoped_method_bound = scoped_method_added &&
      th_unify_in_place(scoped_method_context, scoped_method_receiver,
                        th_name(scoped_method_context, S("int")));
  check("deferred method obligations retain their authoring package",
        scoped_method_bound &&
            th_ctx_set_environment(scoped_method_context, env, S("consumer"),
                                   S("other")) &&
            th_unify(scoped_method_context, scoped_method_result,
                     th_name(scoped_method_context, S("int"))));
  th_ctx *scoped_trait_context = th_ctx_new();
  th_type *scoped_trait_receiver = th_var(scoped_trait_context);
  bool scoped_trait_added = th_ctx_set_scope(
      scoped_trait_context, S("consumer"), S("base")) &&
      th_var_constrain(scoped_trait_context, scoped_trait_receiver,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = S("Readable")});
  bool scoped_trait_bound = scoped_trait_added &&
      th_unify_in_place(scoped_trait_context, scoped_trait_receiver,
                        th_name(scoped_trait_context, S("int")));
  check("deferred trait obligations retain their authoring package",
        scoped_trait_bound &&
            th_ctx_set_environment(scoped_trait_context, env, S("consumer"),
                                   S("other")));
  th_ctx *wrong_scoped_trait_context = th_ctx_new();
  th_type *wrong_scoped_trait_receiver = th_var(wrong_scoped_trait_context);
  bool wrong_scope_added = th_ctx_set_scope(
      wrong_scoped_trait_context, S("consumer"), S("other")) &&
      th_var_constrain(wrong_scoped_trait_context, wrong_scoped_trait_receiver,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = S("Readable")});
  bool wrong_scope_bound = wrong_scope_added &&
      th_unify_in_place(wrong_scoped_trait_context,
                        wrong_scoped_trait_receiver,
                        th_name(wrong_scoped_trait_context, S("int")));
  check("deferred trait obligations do not inherit a later package scope",
        wrong_scope_bound &&
            !th_ctx_set_environment(wrong_scoped_trait_context, env,
                                    S("consumer"), S("base")));
  th_ctx_free(wrong_scoped_trait_context);
  th_ctx_free(scoped_trait_context);
  th_ctx_free(scoped_method_context);
  th_ctx_free(late_invalid_trait_context);
  th_ctx_free(late_trait_context);
  th_ctx_free(late_invalid_context);
  th_ctx_free(late_environment_context);
  check("integer receiver promotes to a matching trait method",
        th_env_method_lookup(inside, th_bitint(inside, 8, true),
                             S("IntegerText"), S("show"), &selected) &&
            core_string_eq(selected.qualified_name, S("base.int16.show")));
  check("least-widening integer receiver candidate wins dispatch",
        th_env_method_lookup(inside, th_bitint(inside, 16, true),
                             S("IntegerText"), S("show"), &selected) &&
            core_string_eq(selected.qualified_name, S("base.int16.show")));
  th_type *consumer_int_array = th_array(inside, consumer_int);
  check("read indexing resolves through the ordinary trait path",
        th_env_implements(inside, consumer_int_array, S("Index_Load")) &&
            th_env_method_lookup(inside, consumer_int_array, S("Index_Load"),
                                 S("load"), &selected) &&
            core_string_eq(selected.qualified_name, S("base.array.load")));
  check("write indexing resolves independently through its trait",
        th_env_implements(inside, consumer_int_array, S("Index_Store")) &&
            th_env_method_lookup(inside, consumer_int_array,
                                 S("Index_Store"), S("store"), &selected) &&
            core_string_eq(selected.qualified_name, S("base.array.store")));
  check("overloaded integer addition uses the same trait dispatch path",
        th_env_method_lookup(inside, consumer_int, S("Add"), S("add"),
                             &selected) &&
            core_string_eq(selected.qualified_name, S("base.int.add")) &&
            !th_env_implements(inside, th_name(inside, S("string")),
                               S("Add")));
  th_env_method_t box_dispatch_candidates[2] = {0};
  size_t box_dispatch_count = th_env_method_candidates(
      inside, S("BoxReadable"), S("get"), box_dispatch_candidates, 2);
  check("trait-value dispatch enumerates generic and concrete candidates",
        box_dispatch_count == 2 &&
            ((core_string_eq(box_dispatch_candidates[0].qualified_name,
                             S("base.box.generic_get")) &&
              core_string_eq(box_dispatch_candidates[1].qualified_name,
                             S("base.box.int_get"))) ||
             (core_string_eq(box_dispatch_candidates[1].qualified_name,
                             S("base.box.generic_get")) &&
              core_string_eq(box_dispatch_candidates[0].qualified_name,
                             S("base.box.int_get")))));
  th_arg dispatch_string_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(inside, S("string")),
  };
  th_type *box_string =
      th_apply(inside, S("Box"), &dispatch_string_arg, 1);
  th_type *possible_box_receivers[] = {box_int, box_string};
  th_env_dispatch_entry_t dispatch_entries[2] = {0};
  size_t dispatch_count = 0;
  bool direct_box_call = true;
  th_env_method_status_t dispatch_status = th_env_method_plan_dispatch(
      inside, S("BoxReadable"), S("get"), possible_box_receivers, 2,
      dispatch_entries, 2, &dispatch_count, &direct_box_call);
  check("known trait-value receiver types produce a complete runtime plan",
        dispatch_status == TH_ENV_METHOD_FOUND && dispatch_count == 2 &&
            !direct_box_call &&
            dispatch_entries[0].receiver_type == box_int &&
            dispatch_entries[1].receiver_type == box_string &&
            core_string_eq(dispatch_entries[0].method.qualified_name,
                           S("base.box.int_get")) &&
            core_string_eq(dispatch_entries[1].method.qualified_name,
                           S("base.box.generic_get")));
  th_env_dispatch_entry_t static_entry = {0};
  bool direct_box_int = false;
  check("a single known runtime receiver reduces to a direct call",
        th_env_method_plan_dispatch(inside, S("BoxReadable"), S("get"),
                                    &box_int, 1, &static_entry, 1,
                                    &dispatch_count,
                                    &direct_box_int) == TH_ENV_METHOD_FOUND &&
            dispatch_count == 1 && direct_box_int &&
            core_string_eq(static_entry.method.qualified_name,
                           S("base.box.int_get")));
  th_env_dispatch_entry_t truncated_entry = {0};
  size_t full_dispatch_count = 0;
  check("dispatch plans report required capacity without losing order",
        th_env_method_plan_dispatch(
            inside, S("BoxReadable"), S("get"), possible_box_receivers, 2,
            &truncated_entry, 1, &full_dispatch_count, nullptr) ==
                TH_ENV_METHOD_FOUND &&
            full_dispatch_count == 2 &&
            truncated_entry.receiver_type == box_int &&
            core_string_eq(truncated_entry.method.qualified_name,
                           S("base.box.int_get")));
  th_type *unsupported_receiver = th_name(inside, S("string"));
  th_env_dispatch_entry_t absent_entry = {0};
  size_t absent_count = 99;
  check("a missing implementation rejects dispatch planning atomically",
        th_env_method_plan_dispatch(inside, S("BoxReadable"), S("get"),
                                    &unsupported_receiver, 1, &absent_entry, 1,
                                    &absent_count, nullptr) ==
                TH_ENV_METHOD_NOT_FOUND &&
            absent_count == 0 && !absent_entry.method.qualified_name.data);
  th_env_dispatch_entry_t untouched_entry = {0};
  size_t rejected_count = 99;
  check("ambiguous runtime receiver types reject the whole dispatch plan",
        th_env_method_plan_dispatch(inside, S("PairReadable"), S("get"),
                                    &concrete_pair, 1, &untouched_entry, 1,
                                    &rejected_count, nullptr) ==
                TH_ENV_METHOD_AMBIGUOUS &&
            rejected_count == 0 &&
            !untouched_entry.method.qualified_name.data);

  check("incomparable overlapping methods are reported as ambiguous",
        th_env_method_resolve(inside, concrete_pair, S("PairReadable"),
                              S("get"), &selected) ==
            TH_ENV_METHOD_AMBIGUOUS);
  th_arg concrete_map_args[] = {
      {.kind = TH_ARG_TYPE, .type = th_name(inside, S("int"))},
      {.kind = TH_ARG_TYPE, .type = th_name(inside, S("string"))},
  };
  th_type *concrete_map = th_apply(inside, S("Map"), concrete_map_args, 2);
  check("generic map receiver satisfies both overlapping candidates",
        th_env_implements(inside, concrete_map, S("MapReadable")));
  check("incomparable map receiver patterns are ambiguous",
        th_env_method_resolve(inside, concrete_map, S("MapReadable"),
                              S("map_get"), &selected) ==
            TH_ENV_METHOD_AMBIGUOUS);
  check("method-name lookup propagates ambiguity across trait contracts",
        th_env_method_resolve_by_name(inside, concrete_pair, S("get"),
                                      &selected, &selected_trait) ==
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
        !th_env_implements(inside, unknown, S("Readable")) &&
            th_resolve(unknown) == unknown);

  th_type *constrained = th_var(inside);
  th_constraint obligation = {
      .kind = TH_CON_IMPLEMENTS,
      .iface = S("Readable"),
  };
  th_var_constrain(inside, constrained, obligation);
  check("trait obligation resolves from function-derived facts",
      th_unify_in_place(inside, constrained, consumer_int) != nullptr);

  th_env *base_library = th_env_new();
  th_ctx *base_library_types = th_ctx_new();
  th_type *fetch_receiver = th_var(base_library_types);
  th_type *string_type = th_name(base_library_types, S("string"));
  th_type *fetch_argument_type = th_name(base_library_types, S("int"));
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
  th_type *fetch_error_type = th_name(base_library_types, S("FetchError"));
  bool deferred_fetch_constrained = th_ctx_set_scope(
      base_library_types, S("traits"), S("base")) && th_var_constrain(
      base_library_types, deferred_fetch_receiver,
      (th_constraint){.kind = TH_CON_METHOD,
                      .method = {.name = S("fetch"),
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
            base_library, S("Bracket_Literal"), S("builtins"), S("base"),
            TH_ENV_PUBLIC, marker_methods) &&
            th_env_register_trait_contracts(
                base_library, S("Brace_Literal"), S("builtins"), S("base"),
                TH_ENV_PUBLIC, marker_methods));
  check("register intrinsic collection-shape type facts",
        th_env_register_intrinsic_type_pattern(
            base_library, S("Bracket_Literal"),
            th_apply(base_library_types, S("list"), &list_pattern_arg, 1)) &&
            th_env_register_intrinsic_type_pattern(
                base_library, S("Bracket_Literal"),
                th_array(base_library_types, array_element)) &&
            th_env_register_intrinsic_type_pattern(
                base_library, S("Brace_Literal"),
                th_apply(base_library_types, S("set"), &set_pattern_arg,
                         1)) &&
            th_env_register_intrinsic_type_pattern(
                base_library, S("Brace_Literal"),
                th_apply(base_library_types, S("dict"), dict_pattern_args,
                         2)));
  th_type *same_trait_receiver = th_var(base_library_types);
  th_type *bound_trait_receiver = th_var(base_library_types);
  th_type *bound_value = th_bound(base_library_types, 1, 0);
  check("register persisted Box type",
        th_env_register_type(base_library, S("Box"), S("types"), S("base"),
                             TH_ENV_PUBLIC));
  th_member box_shape_member = {
      .name = S("value"),
      .type = th_name(base_library_types, S("int")),
  };
  th_type *box_shape = th_record(base_library_types, &box_shape_member, 1);
  check("register a structural definition for a named type",
        box_shape && th_env_register_type_shape(
            base_library, S("Box"), TH_ENV_TYPE_SHAPE_RECORD, box_shape));
  check("register persisted flagset-shaped type",
        th_env_register_type(base_library, S("FlagBits"), S("types"),
                             S("base"), TH_ENV_PUBLIC));
  th_member flag_bits_member = {
      .name = S("enabled"),
      .type = th_name(base_library_types, S("bool")),
  };
  th_type *flag_bits_shape =
      th_record(base_library_types, &flag_bits_member, 1);
  check("register a bitset definition for a named type",
        flag_bits_shape && th_env_register_type_shape(
            base_library, S("FlagBits"), TH_ENV_TYPE_SHAPE_BITSET,
            flag_bits_shape));
  check("register persisted PrivateBox type",
        th_env_register_type(base_library, S("PrivateBox"),
                             S("private_types"), S("base"), TH_ENV_PRIVATE));
  check("register persisted Fetch trait",
        th_env_register_trait_channels(
            base_library, S("Fetch"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(base_library_types, S("fetch"),
                          fn2(base_library_types, fetch_receiver,
                              fetch_argument_type, string_type)),
            method_record(base_library_types, S("fetch"),
                          fetch_error_type)));
  check("register persisted Same trait",
        th_env_register_trait(
            base_library, S("Same"), S("traits"), S("base"), TH_ENV_PUBLIC,
            method_record(base_library_types, S("same"),
                          fn1(base_library_types, same_trait_receiver,
                              same_trait_receiver))));
  check("register persisted BoundSame trait",
        th_env_register_trait(
            base_library, S("BoundSame"), S("traits"), S("base"),
            TH_ENV_PUBLIC,
            method_record(base_library_types, S("bound_same"),
                          fn1(base_library_types, bound_trait_receiver,
                              bound_trait_receiver))));
  check("register persisted DeferredFetch trait",
        deferred_fetch_constrained && th_env_register_trait(
            base_library, S("DeferredFetch"), S("traits"), S("base"),
            TH_ENV_PUBLIC,
            method_record(base_library_types, S("probe"),
                          fn1(base_library_types, deferred_fetch_receiver,
                              deferred_fetch_result))));
  check("register persisted generic Fetch implementation",
        th_env_register_function_channels(
            base_library, S("base.generic.fetch"), S("fetch"), S("generic"),
            S("base"), TH_ENV_PUBLIC,
            fn2(base_library_types,
                th_apply(base_library_types, S("Box"), &link_generic_box_arg,
                         1),
                fetch_argument_type, string_type), fetch_error_type));
  check("register persisted generic identity implementation",
        th_env_register_function(
            base_library, S("base.generic.identity"), S("same"),
            S("generic"), S("base"), TH_ENV_PUBLIC,
            th_generalize(base_library_types,
                fn1(base_library_types, link_generic_element,
                    link_generic_element))));
  check("register persisted bound identity implementation",
        th_env_register_function(
            base_library, S("base.generic.bound_identity"), S("bound_same"),
            S("generic"), S("base"), TH_ENV_PUBLIC,
            th_binder(base_library_types, 1,
                fn1(base_library_types, bound_value, bound_value))));
  check("register a typed external value declaration",
        th_env_register_value(base_library, S("base.counter"), S("counter"),
            S("base"), S("base"), TH_ENV_PUBLIC,
            th_name(base_library_types, S("int"))));
  check("register persisted type layout metadata",
        th_env_register_metadata(base_library, TH_ENV_METADATA_TYPE, S("Box"),
                                 S("layout"), 1, box_layout,
                                 sizeof(box_layout)));
  check("register persisted trait metadata",
        th_env_register_metadata(base_library, TH_ENV_METADATA_TRAIT,
                                 S("Fetch"), S("dispatch"), 1,
                                 fetch_trait_abi, sizeof(fetch_trait_abi)));
  check("register persisted function metadata",
        th_env_register_metadata(base_library, TH_ENV_METADATA_FUNCTION,
                                 S("base.generic.fetch"), S("abi"), 1,
                                 generic_fetch_abi,
                                 sizeof(generic_fetch_abi)));
  check("register value-owned semantic metadata",
        th_env_register_metadata(base_library, TH_ENV_METADATA_VALUE,
            S("base.counter"), S("n00b.global"), 1, global_semantics,
            sizeof(global_semantics)));
  check("register environment manifest metadata",
        th_env_register_metadata(base_library, TH_ENV_METADATA_ENVIRONMENT,
            S("base.manifest"), S("language.package"), 1, "base", 5));
  check("freeze base library for later linking", th_env_freeze(base_library));

  th_arg live_array_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(base_library_types, S("int")),
  };
  th_arg live_list_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(base_library_types, S("int")),
  };
  th_arg live_set_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(base_library_types, S("int")),
  };
  th_arg live_dict_args[] = {
      {.kind = TH_ARG_TYPE,
       .type = th_name(base_library_types, S("string"))},
      {.kind = TH_ARG_TYPE,
       .type = th_name(base_library_types, S("int"))},
  };
  th_type *live_array = th_array(base_library_types, live_array_arg.type);
  th_type *live_list = th_apply(base_library_types, S("list"),
                                &live_list_arg, 1);
  th_type *live_set = th_apply(base_library_types, S("set"), &live_set_arg,
                               1);
  th_type *live_dict = th_apply(base_library_types, S("dict"),
                                live_dict_args, 2);
  th_type *live_bracket_variable = th_var(base_library_types);
  th_type *live_list_variable = th_var(base_library_types);
  th_type *live_brace_variable = th_var(base_library_types);
  th_type *live_dict_variable = th_var(base_library_types);
  bool live_pending_constraint =
      th_var_constrain(base_library_types, live_list_variable,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = S("Bracket_Literal")}) &&
      th_var_constrain(base_library_types, live_bracket_variable,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = S("Bracket_Literal")}) &&
      th_var_constrain(base_library_types, live_brace_variable,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = S("Brace_Literal")}) &&
      th_var_constrain(base_library_types, live_dict_variable,
          (th_constraint){.kind = TH_CON_IMPLEMENTS,
                          .iface = S("Brace_Literal")}) &&
      th_unify_in_place(base_library_types, live_list_variable, live_list) &&
      th_unify_in_place(base_library_types, live_bracket_variable,
                        live_array) &&
      th_unify_in_place(base_library_types, live_brace_variable, live_set) &&
      th_unify_in_place(base_library_types, live_dict_variable, live_dict);
  check("live intrinsic marker facts match in their shared type context",
        live_pending_constraint &&
            th_ctx_set_environment(base_library_types, base_library,
                                   S("client"), S("base")) &&
            th_env_implements(base_library_types, live_array,
                              S("Bracket_Literal")));

  th_ctx *source_check_types = th_ctx_new();
  th_arg source_box_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(source_check_types, S("int")),
  };
  th_env_method_t source_method = {0};
  bool source_dispatch_ok =
      th_ctx_set_environment(source_check_types, base_library, S("client"),
                             S("base")) &&
      th_env_method_lookup(source_check_types,
                           th_apply(source_check_types, S("Box"),
                                    &source_box_arg, 1),
                           S("Fetch"), S("fetch"), &source_method);
  check("source-built library resolves the generic method before saving",
        source_dispatch_ok &&
            core_string_eq(source_method.qualified_name,
                           S("base.generic.fetch")) &&
            th_unify(source_check_types, source_method.error_type,
                     th_name(source_check_types, S("FetchError"))));

  char image_path[] = "/tmp/lantern-th-env-XXXXXX";
  core_string_t image_path_text =
      core_string_borrow(image_path, sizeof(image_path) - 1);
  int image_fd = mkstemp(image_path);
  check("temporary environment image path is available", image_fd >= 0);
  if (image_fd >= 0) {
    close(image_fd);
    unlink(image_path);
  }
  core_object_archive_error_t image_error;
  bool environment_saved = image_fd >= 0 &&
      th_env_save(base_library, image_path_text, 0, &image_error) &&
      image_error == CORE_OBJECT_ARCHIVE_OK;
  check("save frozen trait/function environment and metadata",
        environment_saved);
  bool bad_trait_visibility_saved = environment_saved &&
      mutate_environment_visibility(image_path_text, true, UINT32_MAX);
  th_env *bad_visibility_library = bad_trait_visibility_saved
      ? th_env_load(image_path_text, &image_error) : nullptr;
  check("archive rejects invalid trait visibility values",
        bad_trait_visibility_saved && !bad_visibility_library &&
            image_error == CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
  bool trait_visibility_restored = environment_saved &&
      mutate_environment_visibility(image_path_text, true, TH_ENV_PUBLIC);
  bool bad_function_visibility_saved = trait_visibility_restored &&
      mutate_environment_visibility(image_path_text, false, UINT32_MAX);
  bad_visibility_library = bad_function_visibility_saved
      ? th_env_load(image_path_text, &image_error) : nullptr;
  check("archive rejects invalid function visibility values",
        bad_function_visibility_saved && !bad_visibility_library &&
            image_error == CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
  bool function_visibility_restored = environment_saved &&
      mutate_environment_visibility(image_path_text, false, TH_ENV_PUBLIC);
  bool bad_metadata_owner_saved = function_visibility_restored &&
      mutate_environment_metadata_owner(image_path_text, UINT32_MAX);
  bad_visibility_library = bad_metadata_owner_saved
      ? th_env_load(image_path_text, &image_error) : nullptr;
  check("archive rejects invalid metadata owner tags",
        bad_metadata_owner_saved && !bad_visibility_library &&
            image_error == CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
  check("restore enum bytes after malformed-image checks",
        function_visibility_restored && mutate_environment_metadata_owner(
            image_path_text, TH_ENV_METADATA_TYPE));
  th_env *persisted_library = th_env_load(image_path_text, &image_error);
  check("reload frozen environment from typed archive records",
        persisted_library && th_env_is_frozen(persisted_library) &&
            image_error == CORE_OBJECT_ARCHIVE_OK);
  th_env *image_collision = th_env_load(image_path_text, &image_error);
  check("a second image load restores an independent relocated environment",
        image_collision && image_collision != persisted_library &&
            th_env_is_frozen(image_collision) &&
            image_error == CORE_OBJECT_ARCHIVE_OK);

  th_ctx *persisted_check_types = th_ctx_new();
  th_type *persisted_int = th_name(persisted_check_types, S("int"));
  th_arg persisted_box_arg = {.kind = TH_ARG_TYPE, .type = persisted_int};
  th_type *persisted_box_int =
      th_apply(persisted_check_types, S("Box"), &persisted_box_arg, 1);
  th_env_method_t persisted_method = {0};
  th_env_method_t persisted_identity = {0};
  th_env_method_t persisted_bound_identity = {0};
  th_env_metadata_view_t persisted_layout = {0};
  th_env_metadata_view_t persisted_function_abi = {0};
  th_env_metadata_view_t persisted_value_metadata = {0};
  th_env_metadata_view_t persisted_environment_metadata = {0};
  bool persisted_environment_attached = persisted_library &&
      th_ctx_set_environment(persisted_check_types, persisted_library,
                             S("client"), S("base"));
  th_env_type_shape_kind_t persisted_shape_kind = TH_ENV_TYPE_SHAPE_NONE;
  th_type *persisted_box_shape = persisted_environment_attached
      ? th_env_type_shape(persisted_check_types, S("Box"),
                          &persisted_shape_kind)
      : nullptr;
  th_env_type_shape_kind_t persisted_flag_bits_kind = TH_ENV_TYPE_SHAPE_NONE;
  th_type *persisted_flag_bits_shape = persisted_environment_attached
      ? th_env_type_shape(persisted_check_types, S("FlagBits"),
                          &persisted_flag_bits_kind)
      : nullptr;
  check("loaded library rebuilds trait facts and keeps generic dispatch",
        persisted_library &&
            persisted_environment_attached &&
            persisted_box_shape &&
            persisted_shape_kind == TH_ENV_TYPE_SHAPE_RECORD &&
            th_record_len(persisted_box_shape) == 1 &&
            core_string_eq(th_record_member(persisted_box_shape, 0)->name,
                           S("value")) &&
            th_unify(persisted_check_types,
                th_record_member(persisted_box_shape, 0)->type,
                th_name(persisted_check_types, S("int"))) &&
            th_env_implements(persisted_check_types, persisted_box_int,
                              S("Fetch")) &&
            !th_env_type_visible(persisted_check_types, S("PrivateBox")) &&
            th_env_method_lookup(persisted_check_types, persisted_box_int,
                                 S("Fetch"), S("fetch"), &persisted_method) &&
            core_string_eq(persisted_method.qualified_name,
                           S("base.generic.fetch")) &&
            th_unify(persisted_check_types, persisted_method.error_type,
                     th_name(persisted_check_types, S("FetchError"))) &&
            source_dispatch_ok &&
            core_string_eq(persisted_method.qualified_name,
                           source_method.qualified_name) &&
            th_env_method_lookup(persisted_check_types, persisted_int,
                                 S("Same"), S("same"), &persisted_identity) &&
            core_string_eq(persisted_identity.qualified_name,
                           S("base.generic.identity")));
  check("archive preserves bitset type shapes",
        persisted_flag_bits_shape &&
            persisted_flag_bits_kind == TH_ENV_TYPE_SHAPE_BITSET &&
            th_record_len(persisted_flag_bits_shape) == 1 &&
            core_string_eq(th_record_member(persisted_flag_bits_shape, 0)->name,
                           S("enabled")));
  bool loaded_type_view = false, loaded_trait_view = false;
  bool loaded_function_view = false, loaded_value_view = false;
  for (size_t i = 0; persisted_library &&
       i < th_env_declaration_count(persisted_library); i++) {
    th_env_declaration_view_t declaration = {0};
    if (!th_env_declaration_at(persisted_library, i, &declaration)) continue;
    loaded_type_view |= declaration.kind == TH_ENV_DECLARATION_TYPE &&
        core_string_eq(declaration.qualified_name, S("Box")) &&
        core_string_eq(declaration.package, S("base")) &&
        declaration.type_shape_kind == TH_ENV_TYPE_SHAPE_RECORD &&
        declaration.type_shape == persisted_box_shape;
    loaded_trait_view |= declaration.kind == TH_ENV_DECLARATION_TRAIT &&
        core_string_eq(declaration.qualified_name, S("Fetch")) &&
        declaration.contract != nullptr;
    loaded_function_view |=
        declaration.kind == TH_ENV_DECLARATION_FUNCTION &&
        core_string_eq(declaration.qualified_name, S("base.generic.fetch")) &&
        core_string_eq(declaration.root_name, S("fetch")) &&
        declaration.contract;
    loaded_value_view |= declaration.kind == TH_ENV_DECLARATION_VALUE &&
        core_string_eq(declaration.qualified_name, S("base.counter")) &&
        core_string_eq(declaration.root_name, S("counter")) &&
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
      .type = th_name(persisted_check_types, S("string")),
  };
  th_arg loaded_array_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(persisted_check_types, S("int")),
  };
  th_arg loaded_set_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(persisted_check_types, S("int")),
  };
  th_arg loaded_dict_args[] = {
      {.kind = TH_ARG_TYPE,
       .type = th_name(persisted_check_types, S("string"))},
      {.kind = TH_ARG_TYPE, .type = th_name(persisted_check_types, S("int"))},
  };
  th_arg loaded_tuple_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(persisted_check_types, S("int")),
  };
  check("loaded intrinsic marker facts validate collection shapes",
        persisted_library &&
            th_env_implements(persisted_check_types,
                th_apply(persisted_check_types, S("list"), &loaded_list_arg,
                         1),
                S("Bracket_Literal")) &&
            th_env_implements(persisted_check_types,
                th_array(persisted_check_types, loaded_array_arg.type),
                S("Bracket_Literal")) &&
            th_env_implements(persisted_check_types,
                th_apply(persisted_check_types, S("set"), &loaded_set_arg, 1),
                S("Brace_Literal")) &&
            th_env_implements(persisted_check_types,
                th_apply(persisted_check_types, S("dict"), loaded_dict_args,
                         2),
                S("Brace_Literal")) &&
            !th_env_implements(persisted_check_types,
                th_apply(persisted_check_types, S("tuple"),
                         &loaded_tuple_arg, 1),
                S("Bracket_Literal")));
  th_type *empty_brace_type = th_var(persisted_check_types);
  check("empty brace trait constraint accepts a linked dictionary type",
        th_var_constrain(persisted_check_types, empty_brace_type,
            (th_constraint){.kind = TH_CON_IMPLEMENTS,
                            .iface = S("Brace_Literal")}) &&
            th_unify_in_place(persisted_check_types, empty_brace_type,
                th_apply(persisted_check_types, S("dict"), loaded_dict_args,
                         2)));
  th_ctx *wrong_shape_types = th_ctx_new();
  th_ctx_set_environment(wrong_shape_types, persisted_library, S("client"),
                         S("base"));
  th_type *wrong_brace_type = th_var(wrong_shape_types);
  th_arg wrong_shape_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(wrong_shape_types, S("int")),
  };
  check("empty brace trait constraint rejects an unrelated type",
        th_var_constrain(wrong_shape_types, wrong_brace_type,
            (th_constraint){.kind = TH_CON_IMPLEMENTS,
                            .iface = S("Brace_Literal")}) &&
            !th_unify_in_place(wrong_shape_types, wrong_brace_type,
                th_apply(wrong_shape_types, S("tuple"), &wrong_shape_arg,
                         1)));
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
          literal_extension, S("Bracket_Literal"), S("builtins"), S("base"),
          TH_ENV_PUBLIC, extension_marker_methods) &&
      th_env_register_intrinsic_type_pattern(
          literal_extension, S("Bracket_Literal"),
          th_apply(literal_extension_types, S("Vector"), &vector_pattern_arg,
                   1)) &&
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
      .type = th_name(literal_composed_types, S("string")),
  };
  th_arg literal_dict_args[] = {
      {.kind = TH_ARG_TYPE,
       .type = th_name(literal_composed_types, S("string"))},
      {.kind = TH_ARG_TYPE, .type = th_name(literal_composed_types, S("int"))},
  };
  bool literal_context_ready = literal_composed_ready &&
      th_ctx_set_environment(literal_composed_types, literal_composed,
                             S("client"), S("base"));
  check("attach the composed intrinsic environment", literal_context_ready);
  check("link open builtin-shape facts across matching marker traits",
        literal_context_ready &&
            th_env_implements(literal_composed_types,
                th_apply(literal_composed_types, S("Vector"),
                         &literal_vector_arg, 1), S("Bracket_Literal")) &&
            th_env_implements(literal_composed_types,
                th_apply(literal_composed_types, S("dict"), literal_dict_args,
                         2),
                S("Brace_Literal")));
  th_arg second_vector_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(literal_composed_types, S("int")),
  };
  check("intrinsic trait matching does not specialize its stored pattern",
        literal_context_ready &&
            th_env_implements(literal_composed_types,
                th_apply(literal_composed_types, S("Vector"),
                         &second_vector_arg, 1), S("Bracket_Literal")));
  check("loader retains binders without an optional quantified-variable list",
        persisted_library &&
            th_env_method_lookup(persisted_check_types, persisted_int,
                                 S("BoundSame"), S("bound_same"),
                                 &persisted_bound_identity) &&
            core_string_eq(persisted_bound_identity.qualified_name,
                           S("base.generic.bound_identity")));
  th_env_method_requirement_t deferred_requirement = {0};
  bool deferred_requirement_loaded = persisted_library &&
      th_env_method_requirements(persisted_check_types, S("probe"),
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
      core_string_eq(loaded_deferred_constraint.scope_module, S("traits")) &&
      core_string_eq(loaded_deferred_constraint.scope_package, S("base")) &&
      core_string_eq(loaded_deferred_constraint.method.name, S("fetch")) &&
      loaded_deferred_constraint.method.result == loaded_deferred_result &&
      loaded_deferred_constraint.method.call &&
      th_func_nparams(loaded_deferred_constraint.method.call) == 1;
  check("archive preserves deferred method call arguments and result linkage",
        has_deferred_constraint &&
            th_unify_in_place(persisted_check_types,
                              loaded_deferred_receiver, persisted_box_int) &&
            th_unify(persisted_check_types, loaded_deferred_result,
                     th_name(persisted_check_types, S("string"))));
  th_ctx *bad_method_call_context = th_ctx_new();
  th_ctx_set_environment(bad_method_call_context, persisted_library,
                         S("client"), S("base"));
  th_type *bad_method_receiver = th_var(bad_method_call_context);
  th_type *bad_method_result = th_var(bad_method_call_context);
  th_param bad_method_argument = {
      .mode = TH_IN,
      .type = th_name(bad_method_call_context, S("string")),
  };
  th_type *bad_method_call = th_func(
      bad_method_call_context, bad_method_result, &bad_method_argument, 1,
      nullptr, nullptr, 0);
  bool bad_method_obligation = th_var_constrain(
      bad_method_call_context, bad_method_receiver,
      (th_constraint){.kind = TH_CON_METHOD,
                      .method = {.name = S("fetch"),
                                 .result = bad_method_result,
                                 .call = bad_method_call}});
  check("method obligations reject mismatched argument types",
        bad_method_obligation &&
            !th_unify_in_place(bad_method_call_context,
                               bad_method_receiver, persisted_box_int));
  th_ctx_free(bad_method_call_context);
  th_ctx *method_error_context = th_ctx_new();
  th_ctx_set_environment(method_error_context, persisted_library,
                         S("client"), S("base"));
  th_type *method_error_receiver = th_var(method_error_context);
  th_type *method_error_result = th_var(method_error_context);
  th_type *method_error = th_var(method_error_context);
  th_param method_error_argument = {
      .mode = TH_IN, .type = th_name(method_error_context, S("int")),
  };
  th_type *method_error_call = th_func(method_error_context,
      method_error_result, &method_error_argument, 1, nullptr, nullptr, 0);
  bool method_error_required = th_require_method(method_error_context,
      method_error_receiver, S("fetch"), method_error_result,
      method_error_call);
  bool method_error_connected = th_method_obligation_set_error(
      method_error_context, method_error_receiver, S("fetch"),
      method_error_result, method_error);
  th_param method_error_parameter = {
      .mode = TH_IN, .type = method_error_receiver,
  };
  th_type *method_error_function = th_func_with_error(method_error_context,
      method_error_result, method_error, &method_error_parameter, 1,
      nullptr, nullptr, 0);
  th_param concrete_method_parameter = {
      .mode = TH_IN,
  };
  th_arg method_error_box_argument = {
      .kind = TH_ARG_TYPE, .type = th_name(method_error_context, S("int")),
  };
  th_type *method_error_box = th_apply(method_error_context, S("Box"),
                                       &method_error_box_argument, 1);
  concrete_method_parameter.type = method_error_box;
  th_type *concrete_method_function = th_func_with_error(method_error_context,
      th_name(method_error_context, S("string")),
      th_name(method_error_context, S("FetchError")),
      &concrete_method_parameter, 1, nullptr, nullptr, 0);
  check("copying method obligations connects the failure channel without refining the contract",
        method_error_required && method_error_connected
        && th_var_constraint_count(method_error_receiver) == 1
        && th_unify(method_error_context,
            method_error_function, concrete_method_function) &&
        th_kind_of(th_resolve(method_error_receiver)) == TH_VAR &&
        th_kind_of(th_resolve(method_error_result)) == TH_VAR &&
        th_kind_of(th_resolve(method_error)) == TH_VAR);
  th_type *method_error_scheme = th_generalize(method_error_context,
                                               method_error_function);
  th_type *method_error_receiver_alias = th_var(method_error_context);
  th_type *method_error_result_alias = th_var(method_error_context);
  th_type *method_error_failure_alias = th_var(method_error_context);
  bool method_error_scheme_aliased = method_error_scheme
      && th_unify_in_place(method_error_context, method_error_receiver,
                           method_error_receiver_alias)
      && th_unify_in_place(method_error_context, method_error_result,
                           method_error_result_alias)
      && th_unify_in_place(method_error_context, method_error,
                           method_error_failure_alias);
  th_type *method_error_instance = th_instantiate(method_error_context,
                                                  method_error_scheme);
  check("instantiated method obligations infer the selected failure channel",
        method_error_instance && th_unify_in_place(method_error_context,
            (th_type *)th_func_param(method_error_instance, 0)->type,
            method_error_box) &&
        th_unify(method_error_context, th_func_error(method_error_instance),
                 th_name(method_error_context, S("FetchError"))) &&
        !th_unify(method_error_context, th_func_error(method_error_instance),
                  th_name(method_error_context, S("nil"))));
  th_type *independent_method_error_instance = th_instantiate(
      method_error_context, method_error_scheme);
  check("quantified method obligations remain independent through variable aliases",
        method_error_scheme_aliased && independent_method_error_instance
        && th_kind_of(th_resolve((th_type *)th_func_param(
               independent_method_error_instance, 0)->type)) == TH_VAR
        && th_kind_of(th_resolve(th_func_ret(
               independent_method_error_instance))) == TH_VAR
        && th_kind_of(th_resolve(th_func_error(
               independent_method_error_instance))) == TH_VAR);
  th_type *bad_method_error = th_func_with_error(method_error_context,
      th_name(method_error_context, S("string")),
      th_name(method_error_context, S("int")),
      &concrete_method_parameter, 1, nullptr, nullptr, 0);
  check("method obligations reject an incompatible failure channel",
        !th_unify(method_error_context, method_error_function, bad_method_error));
  th_type *unary_receiver = th_var(method_error_context);
  th_type *unary_result = th_var(method_error_context);
  th_type *unary_error = th_var(method_error_context);
  th_constraint unary_constraint = {0};
  bool unary_required = th_require_method(method_error_context,
      unary_receiver, S("negate"), unary_result, nullptr);
  bool unary_connected = th_method_obligation_set_error(method_error_context,
      unary_receiver, S("negate"), unary_result, unary_error);
  check("unary method obligations retain a zero-argument failure channel",
        unary_required && unary_connected
        && th_var_constraint_at(unary_receiver, 0, &unary_constraint)
        && unary_constraint.kind == TH_CON_METHOD
        && unary_constraint.method.call
        && th_func_nparams(unary_constraint.method.call) == 0
        && th_func_error(unary_constraint.method.call) == unary_error);
  th_type *repeated_method_receiver = th_var(method_error_context);
  th_type *repeated_result = th_var(method_error_context);
  th_type *repeated_error = th_name(method_error_context, S("nil"));
  th_type *partial_call = th_func(method_error_context, repeated_result,
                                  nullptr, 0, nullptr, nullptr, 0);
  th_type *complete_call = th_func_with_error(method_error_context,
      repeated_result, repeated_error, nullptr, 0, nullptr, nullptr, 0);
  bool repeated_required = th_require_method(method_error_context,
      repeated_method_receiver, S("render"), repeated_result, partial_call)
      && th_require_method(method_error_context, repeated_method_receiver,
                           S("render"), repeated_result, complete_call);
  bool repeated_connected = th_method_obligation_set_error(
      method_error_context, repeated_method_receiver, S("render"),
      repeated_result, repeated_error);
  bool repeated_complete = repeated_required && repeated_connected
      && th_var_constraint_count(repeated_method_receiver) == 1;
  for (size_t i = 0; repeated_complete && i < 1; i++) {
      th_constraint constraint;
      repeated_complete = th_var_constraint_at(repeated_method_receiver, i,
          &constraint) && constraint.kind == TH_CON_METHOD
          && th_func_error(constraint.method.call) == repeated_error;
  }
  check("matching partial and complete method obligations share one slot",
        repeated_complete);
  th_type *overloaded_receiver = th_var(method_error_context);
  th_type *integer_result = th_name(method_error_context, S("int"));
  th_type *string_result = th_name(method_error_context, S("string"));
  th_type *integer_call = th_func(method_error_context, integer_result,
                                 nullptr, 0, nullptr, nullptr, 0);
  th_type *string_call = th_func(method_error_context, string_result,
                                nullptr, 0, nullptr, nullptr, 0);
  check("repeated method overload matches beyond the first slot",
        th_require_method(method_error_context, overloaded_receiver,
            S("render"), integer_result, integer_call)
        && th_require_method(method_error_context, overloaded_receiver,
            S("render"), string_result, string_call)
        && th_require_method(method_error_context, overloaded_receiver,
            S("render"), string_result, string_call)
        && th_var_constraint_count(overloaded_receiver) == 2);
  th_type *fresh_receiver = th_var(method_error_context);
  th_type *fresh_first = th_var(method_error_context);
  th_type *fresh_second = th_var(method_error_context);
  th_type *fresh_first_call = th_func(method_error_context, fresh_first,
                                     nullptr, 0, nullptr, nullptr, 0);
  th_type *fresh_second_call = th_func(method_error_context, fresh_second,
                                      nullptr, 0, nullptr, nullptr, 0);
  check("repeated method calls unify fresh result variables",
        th_require_method(method_error_context, fresh_receiver, S("hash"),
            fresh_first, fresh_first_call)
        && th_require_method(method_error_context, fresh_receiver, S("hash"),
            fresh_second, fresh_second_call)
        && th_var_constraint_count(fresh_receiver) == 1
        && th_resolve(fresh_first) == th_resolve(fresh_second));
  th_type *merge_partial = th_var(method_error_context);
  th_type *merge_complete = th_var(method_error_context);
  th_type *merge_result = th_name(method_error_context, S("string"));
  th_type *merge_partial_call = th_func(method_error_context, merge_result,
                                        nullptr, 0, nullptr, nullptr, 0);
  th_type *merge_complete_call = th_func_with_error(method_error_context,
      merge_result, repeated_error, nullptr, 0, nullptr, nullptr, 0);
  bool merge_required = th_require_method(method_error_context, merge_partial,
      S("render"), merge_result, merge_partial_call)
      && th_require_method(method_error_context, merge_complete, S("render"),
                           merge_result, merge_complete_call);
  th_constraint merged_constraint = {0};
  check("unifying method obligations completes the failure channel",
        merge_required
        && th_unify_in_place(method_error_context, merge_partial,
                             merge_complete)
        && th_var_constraint_count(th_resolve(merge_partial)) == 1
        && th_var_constraint_at(th_resolve(merge_partial), 0,
                                &merged_constraint)
        && th_func_error(merged_constraint.method.call) == repeated_error);
  th_ctx_free(method_error_context);
  th_ctx_free(wrong_shape_types);
  check("loaded image preserves type layout and function ABI metadata",
        persisted_library &&
            th_env_metadata_count(persisted_library) == 5 &&
            th_env_metadata_at(persisted_library, 0, &persisted_layout) &&
            persisted_layout.owner_kind == TH_ENV_METADATA_TYPE &&
            core_string_eq(persisted_layout.owner_name, S("Box")) &&
            persisted_layout.version == 1 &&
            persisted_layout.size == sizeof(box_layout) &&
            !memcmp(persisted_layout.data, box_layout, sizeof(box_layout)) &&
            th_env_metadata_at(persisted_library, 2,
                               &persisted_function_abi) &&
            persisted_function_abi.owner_kind == TH_ENV_METADATA_FUNCTION &&
            core_string_eq(persisted_function_abi.owner_name,
                           S("base.generic.fetch")) &&
            !memcmp(persisted_function_abi.data, generic_fetch_abi,
                    sizeof(generic_fetch_abi)) &&
            th_env_metadata_at(persisted_library, 3,
                               &persisted_value_metadata) &&
            persisted_value_metadata.owner_kind == TH_ENV_METADATA_VALUE &&
            core_string_eq(persisted_value_metadata.owner_name,
                           S("base.counter")) &&
            !memcmp(persisted_value_metadata.data, global_semantics,
                    sizeof(global_semantics)) &&
            th_env_metadata_at(persisted_library, 4,
                               &persisted_environment_metadata) &&
            persisted_environment_metadata.owner_kind ==
                TH_ENV_METADATA_ENVIRONMENT &&
            core_string_eq(persisted_environment_metadata.owner_name,
                           S("base.manifest")) &&
            core_string_eq(persisted_environment_metadata.key,
                           S("language.package")));

  const th_env *duplicate_import_inputs[] = {base_library,
                                             persisted_library};
  th_env_compose_error_t duplicate_import_error = {0};
  th_env *duplicate_imports = th_env_compose_diagnose(
      duplicate_import_inputs, 2, &duplicate_import_error);
  check("compose identical source and archived module declarations",
        duplicate_imports &&
            duplicate_import_error.status == TH_ENV_COMPOSE_OK);
  th_env_free(duplicate_imports);

  th_env *conflicting_library = th_env_new();
  check("build library with a duplicate function identity",
        th_env_register_function(
            conflicting_library, S("base.generic.fetch"), S("fetch"),
            S("other"), S("other"), TH_ENV_PUBLIC,
            fn0(base_library_types, th_name(base_library_types, S("int")))) &&
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
            && core_string_eq(compose_error.identity,
                              S("base.generic.fetch")));
  check("link conflict is also reported through core diagnostics",
        link_diag && link_diag->severity == CORE_DIAG_ERROR &&
            core_string_eq(link_diag->code,
                           CORE_STRING_STATIC("TH-L0004")) &&
            core_string_find(link_diag->message,
                             S("base.generic.fetch")) >= 0);
  core_diagnostics_free(link_diagnostics);
  th_env_free(conflicting_library);

  th_env *metadata_conflict_library = th_env_new();
  const uint8_t alternate_box_layout[] = {1, 16, 8, 0};
  check("build library with conflicting type metadata",
        th_env_register_type(metadata_conflict_library, S("Box"), S("types"),
                             S("base"), TH_ENV_PUBLIC) &&
            th_env_register_metadata(metadata_conflict_library,
                                     TH_ENV_METADATA_TYPE, S("Box"),
                                     S("layout"), 1, alternate_box_layout,
                                     sizeof(alternate_box_layout)) &&
            th_env_freeze(metadata_conflict_library));
  const th_env *metadata_conflict_inputs[] = {base_library,
                                               metadata_conflict_library};
  conflicted = th_env_compose_diagnose(metadata_conflict_inputs, 2,
                                       &compose_error);
  check("link conflict identifies metadata owner and key",
        !conflicted &&
            compose_error.status == TH_ENV_COMPOSE_METADATA_CONFLICT &&
            core_string_eq(compose_error.identity, S("Box")) &&
            core_string_eq(compose_error.metadata_key, S("layout")));
  th_env_free(metadata_conflict_library);

  th_ctx *conflicting_shape_types = th_ctx_new();
  th_env *shape_conflict_library = th_env_new();
  th_member conflicting_shape_member = {
      .name = S("other"),
      .type = th_name(conflicting_shape_types, S("string")),
  };
  th_type *conflicting_shape = th_record(conflicting_shape_types,
      &conflicting_shape_member, 1);
  check("build library with conflicting named type shape",
        th_env_register_type(shape_conflict_library, S("Box"), S("types"),
                             S("base"), TH_ENV_PUBLIC) &&
            th_env_register_type_shape(shape_conflict_library, S("Box"),
                TH_ENV_TYPE_SHAPE_RECORD, conflicting_shape) &&
            th_env_freeze(shape_conflict_library));
  const th_env *shape_conflict_inputs[] = {base_library,
                                           shape_conflict_library};
  conflicted = th_env_compose_diagnose(shape_conflict_inputs, 2,
                                       &compose_error);
  check("link conflict identifies incompatible named type shapes",
        !conflicted && compose_error.status == TH_ENV_COMPOSE_TYPE_CONFLICT &&
            core_string_eq(compose_error.identity, S("Box")));
  th_env_free(shape_conflict_library);
  th_ctx_free(conflicting_shape_types);

  th_ctx *generic_left_types = th_ctx_new();
  th_ctx *generic_right_types = th_ctx_new();
  th_env *generic_left = th_env_new_in_context(generic_left_types);
  th_env *generic_right = th_env_new_in_context(generic_right_types);
  th_type *left_item = th_var(generic_left_types);
  th_type *right_item = th_var(generic_right_types);
  th_member left_fields[] = {
      {.name = S("first"), .type = left_item},
      {.name = S("second"), .type = left_item},
  };
  th_member right_fields[] = {
      {.name = S("first"), .type = right_item},
      {.name = S("second"), .type = right_item},
  };
  check("register matching generic shapes in independent environments",
        th_env_register_type(generic_left, S("Pair"), S("generic"),
                             S("base"), TH_ENV_PUBLIC) &&
            th_env_register_type_shape(generic_left, S("Pair"),
                TH_ENV_TYPE_SHAPE_RECORD,
                th_record(generic_left_types, left_fields, 2)) &&
            th_env_freeze(generic_left) &&
            th_env_register_type(generic_right, S("Pair"), S("generic"),
                                 S("base"), TH_ENV_PUBLIC) &&
            th_env_register_type_shape(generic_right, S("Pair"),
                TH_ENV_TYPE_SHAPE_RECORD,
                th_record(generic_right_types, right_fields, 2)) &&
            th_env_freeze(generic_right));
  const th_env *matching_generic_inputs[] = {generic_left, generic_right};
  th_env *matching_generic = th_env_compose_diagnose(
      matching_generic_inputs, 2, &compose_error);
  check("matching generic shapes compose across type contexts",
        matching_generic && compose_error.status == TH_ENV_COMPOSE_OK);
  th_env_free(matching_generic);

  th_ctx *generic_mismatch_types = th_ctx_new();
  th_env *generic_mismatch = th_env_new_in_context(generic_mismatch_types);
  th_member mismatched_fields[] = {
      {.name = S("first"), .type = th_var(generic_mismatch_types)},
      {.name = S("second"), .type = th_var(generic_mismatch_types)},
  };
  check("register generic shape with distinct field variables",
        th_env_register_type(generic_mismatch, S("Pair"), S("generic"),
                             S("base"), TH_ENV_PUBLIC) &&
            th_env_register_type_shape(generic_mismatch, S("Pair"),
                TH_ENV_TYPE_SHAPE_RECORD,
                th_record(generic_mismatch_types, mismatched_fields, 2)) &&
            th_env_freeze(generic_mismatch));
  const th_env *mismatched_generic_inputs[] = {generic_left,
                                               generic_mismatch};
  th_env *mismatched_generic = th_env_compose_diagnose(
      mismatched_generic_inputs, 2, &compose_error);
  check("generic variable sharing remains part of shape identity",
        !mismatched_generic &&
            compose_error.status == TH_ENV_COMPOSE_TYPE_CONFLICT);
  th_env_free(mismatched_generic);
  th_env_free(generic_mismatch);
  th_ctx_free(generic_mismatch_types);
  th_env_free(generic_right);
  th_env_free(generic_left);
  th_ctx_free(generic_right_types);
  th_ctx_free(generic_left_types);

  const th_env *base_libraries[] = {persisted_library};
  th_env *linked_library = th_env_compose(base_libraries, 1);
  th_ctx *linked_types = th_ctx_new();
  th_type *linked_int = th_name(linked_types, S("int"));
  th_arg linked_box_arg = {.kind = TH_ARG_TYPE, .type = linked_int};
  th_type *linked_box_int =
      th_apply(linked_types, S("Box"), &linked_box_arg, 1);
  check("link module contributions into a new environment snapshot",
        linked_library &&
            th_env_register_function_channels(
                linked_library, S("plugin.int_box.fetch"), S("fetch"),
                S("int-box"), S("plugin"), TH_ENV_PUBLIC,
                fn2(linked_types, linked_box_int,
                    th_name(linked_types, S("int")),
                    th_name(linked_types, S("string"))),
                th_name(linked_types, S("FetchError"))) &&
            th_env_register_metadata(linked_library,
                                     TH_ENV_METADATA_FUNCTION,
                                     S("plugin.int_box.fetch"), S("abi"), 1,
                                     generic_fetch_abi,
                                     sizeof(generic_fetch_abi)) &&
            th_env_freeze(linked_library));

  th_ctx_set_environment(linked_types, linked_library, S("client"),
                         S("base"));
  th_env_type_shape_kind_t linked_shape_kind = TH_ENV_TYPE_SHAPE_NONE;
  th_type *linked_box_shape = th_env_type_shape(linked_types, S("Box"),
                                                &linked_shape_kind);
  check("composed snapshots retain canonical named type shapes",
        linked_box_shape && linked_shape_kind == TH_ENV_TYPE_SHAPE_RECORD &&
            th_record_len(linked_box_shape) == 1 &&
            core_string_eq(th_record_member(linked_box_shape, 0)->name,
                           S("value")));

  th_env_metadata_view_t layout_metadata = {0};
  th_env_metadata_view_t function_metadata = {0};
  check("type, function, and value metadata survive environment composition",
        th_env_metadata_count(linked_library) == 6 &&
            th_env_metadata_at(linked_library, 0, &layout_metadata) &&
            layout_metadata.owner_kind == TH_ENV_METADATA_TYPE &&
            core_string_eq(layout_metadata.owner_name, S("Box")) &&
            core_string_eq(layout_metadata.key, S("layout")) &&
            layout_metadata.version == 1 &&
            layout_metadata.size == sizeof(box_layout) &&
            !memcmp(layout_metadata.data, box_layout, sizeof(box_layout)) &&
            th_env_metadata_at(linked_library, 1, &function_metadata) &&
            function_metadata.owner_kind == TH_ENV_METADATA_TRAIT &&
            core_string_eq(function_metadata.owner_name, S("Fetch")) &&
            core_string_eq(function_metadata.key, S("dispatch")) &&
            function_metadata.size == sizeof(fetch_trait_abi) &&
            !memcmp(function_metadata.data, fetch_trait_abi,
                    sizeof(fetch_trait_abi)) &&
            th_env_metadata_at(linked_library, 2, &function_metadata) &&
            function_metadata.owner_kind == TH_ENV_METADATA_FUNCTION &&
            core_string_eq(function_metadata.owner_name,
                           S("base.generic.fetch")) &&
            core_string_eq(function_metadata.key, S("abi")) &&
            th_env_metadata_at(linked_library, 3, &function_metadata) &&
            function_metadata.owner_kind == TH_ENV_METADATA_VALUE &&
            core_string_eq(function_metadata.owner_name, S("base.counter")) &&
            core_string_eq(function_metadata.key, S("n00b.global")) &&
            th_env_metadata_at(linked_library, 4, &function_metadata) &&
            function_metadata.owner_kind == TH_ENV_METADATA_ENVIRONMENT &&
            core_string_eq(function_metadata.owner_name,
                           S("base.manifest")) &&
            core_string_eq(function_metadata.key, S("language.package")) &&
            th_env_metadata_at(linked_library, 5, &function_metadata) &&
            function_metadata.owner_kind == TH_ENV_METADATA_FUNCTION &&
            core_string_eq(function_metadata.owner_name,
                           S("plugin.int_box.fetch")) &&
            core_string_eq(function_metadata.key, S("abi")));
  th_ctx *base_consumer = th_ctx_new();
  th_ctx *linked_consumer = th_ctx_new();
  th_env_method_t base_method = {0};
  th_env_method_t linked_method = {0};
  th_arg base_box_arg = {
      .kind = TH_ARG_TYPE,
      .type = th_name(base_consumer, S("int")),
  };
  check("link-time additions specialize dispatch without mutating libraries",
        linked_library &&
            th_ctx_set_environment(base_consumer, base_library, S("client"),
                                   S("base")) &&
            th_ctx_set_environment(linked_consumer, linked_library,
                                   S("client"), S("base")) &&
            th_env_method_lookup(
                base_consumer,
                th_apply(base_consumer, S("Box"), &base_box_arg, 1),
                S("Fetch"), S("fetch"), &base_method) &&
            th_env_method_lookup(linked_consumer, linked_box_int, S("Fetch"),
                                 S("fetch"), &linked_method) &&
            core_string_eq(base_method.qualified_name,
                           S("base.generic.fetch")) &&
            th_unify(base_consumer, base_method.error_type,
                     th_name(base_consumer, S("FetchError"))) &&
            core_string_eq(linked_method.qualified_name,
                           S("plugin.int_box.fetch")));

  th_env *replacement_env = th_env_new();
  const uint8_t provisional_metadata[] = {1, 2};
  const uint8_t resolved_metadata[] = {3, 4, 5};
  check("register provisional environment metadata",
        replacement_env && th_env_register_metadata(replacement_env,
            TH_ENV_METADATA_ENVIRONMENT, S("phase"), S("result"), 1,
            provisional_metadata, sizeof(provisional_metadata)));
  check("replacement requires an existing metadata key",
        replacement_env && !th_env_replace_metadata(replacement_env,
            TH_ENV_METADATA_ENVIRONMENT, S("phase"), S("missing"), 1,
            resolved_metadata, sizeof(resolved_metadata)));
  check("replace provisional environment metadata",
        replacement_env && th_env_replace_metadata(replacement_env,
            TH_ENV_METADATA_ENVIRONMENT, S("phase"), S("result"), 2,
            resolved_metadata, sizeof(resolved_metadata)));
  th_env_metadata_view_t replacement_view = {0};
  check("replacement retains one current metadata record",
        replacement_env && th_env_metadata_count(replacement_env) == 1 &&
            th_env_metadata_at(replacement_env, 0, &replacement_view) &&
            replacement_view.version == 2 &&
            replacement_view.size == sizeof(resolved_metadata) &&
            !memcmp(replacement_view.data, resolved_metadata,
                    sizeof(resolved_metadata)));
  check("frozen metadata cannot be replaced",
        replacement_env && th_env_freeze(replacement_env) &&
            !th_env_replace_metadata(replacement_env,
                TH_ENV_METADATA_ENVIRONMENT, S("phase"), S("result"), 1,
                provisional_metadata, sizeof(provisional_metadata)));
  char replacement_path[] = "/tmp/lantern-th-meta-XXXXXX";
  core_string_t replacement_path_text =
      core_string_borrow(replacement_path, sizeof(replacement_path) - 1);
  int replacement_fd = mkstemp(replacement_path);
  if (replacement_fd >= 0) {
    close(replacement_fd);
    unlink(replacement_path);
  }
  th_env *replacement_loaded = nullptr;
  if (replacement_fd >= 0 && replacement_env) {
    core_object_archive_error_t replacement_error = CORE_OBJECT_ARCHIVE_OK;
    if (th_env_save(replacement_env, replacement_path_text, 0,
                    &replacement_error))
      replacement_loaded = th_env_load(replacement_path_text,
                                       &replacement_error);
  }
  replacement_view = (th_env_metadata_view_t){0};
  check("replaced metadata survives archive round trip",
        replacement_loaded && th_env_metadata_count(replacement_loaded) == 1 &&
            th_env_metadata_at(replacement_loaded, 0, &replacement_view) &&
            replacement_view.version == 2 &&
            replacement_view.size == sizeof(resolved_metadata) &&
            !memcmp(replacement_view.data, resolved_metadata,
                    sizeof(resolved_metadata)));
  th_env_free(replacement_loaded);
  th_env_free(replacement_env);
  if (replacement_fd >= 0) unlink(replacement_path);

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
