#define _XOPEN_SOURCE 700

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "lib/object_archive.h"

#define TEST_NODE_TYPE UINT64_C(0x746573742e6e6f64)
#define TEST_ARCHIVE_ABI UINT64_C(0x746573742e617263)

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

static size_t saved_first_payload_size;

static bool mutate_first_record(const char *path, uint64_t field,
                                size_t payload_size)
{
  core_persist_arena_t *arena = nullptr;
  core_persist_arena_error_t error;
  if (!core_persist_arena_open(path, TEST_ARCHIVE_ABI, true, &arena, &error))
    return false;
  test_archive_image_t *image = core_persist_arena_root(arena);
  bool valid = core_persist_arena_contains(arena, image, sizeof(*image)) &&
               image->record_count &&
               core_persist_arena_contains(
                   arena, image->records, sizeof(test_archive_record_t));
  if (valid) {
    test_archive_record_t *records =
        (test_archive_record_t *)image->records;
    if (!saved_first_payload_size)
      saved_first_payload_size = records[0].payload_size;
    records[0].id = field;
    records[0].payload_size = payload_size ? payload_size
                                           : saved_first_payload_size;
    valid = core_persist_arena_sync(arena, &error);
  }
  bool closed = core_persist_arena_close(arena, &error);
  return valid && closed;
}

typedef struct test_node {
  uint64_t value;
  char *label;
  struct test_node *left;
  struct test_node *right;
} test_node_t;

static size_t node_destroy_calls;
static bool reject_node_restore;

static void *node_allocate(core_persist_arena_t *arena, uint32_t version,
                           core_object_reader_t *payload, void *user_data) {
  (void)payload;
  (void)user_data;
  if (version != 1)
    return nullptr;
  return core_persist_arena_alloc_transient(
      arena, sizeof(test_node_t), alignof(test_node_t), nullptr);
}

static bool node_save(core_object_writer_t *writer, const void *object,
                      void *user_data) {
  (void)user_data;
  const test_node_t *node = object;
  return core_object_writer_u64(writer, node->value) &&
         core_object_writer_string(writer, node->label) &&
         core_object_writer_ref(writer, TEST_NODE_TYPE, node->left) &&
         core_object_writer_ref(writer, TEST_NODE_TYPE, node->right) &&
         core_object_writer_ref(writer, 0, nullptr);
}

static bool node_restore(core_object_reader_t *reader, void *object,
                         void *user_data) {
  if (user_data && reject_node_restore)
    return false;
  test_node_t *node = object;
  void *left = nullptr, *right = nullptr, *absent = nullptr;
  if (!core_object_reader_u64(reader, &node->value) ||
      !core_object_reader_string(reader, &node->label) ||
      !core_object_reader_ref(reader, 0, &left) ||
      !core_object_reader_ref(reader, TEST_NODE_TYPE, &right) ||
      !core_object_reader_ref(reader, 0, &absent) || absent)
    return false;
  node->left = left;
  node->right = right;
  return true;
}

static void node_destroy(void *object, void *user_data) {
  (void)object;
  (void)user_data;
  node_destroy_calls++;
}

int main(void) {
  char path[] = "/tmp/lantern-object-archive-XXXXXX";
  char path2[] = "/tmp/lantern-object-archive-second-XXXXXX";
  int fd = mkstemp(path);
  int fd2 = mkstemp(path2);
  check("temporary archive path is available", fd >= 0);
  check("second temporary archive path is available", fd2 >= 0);
  if (fd < 0 || fd2 < 0)
    return report("object archive");
  close(fd);
  close(fd2);
  unlink(path);
  unlink(path2);

  test_node_t shared = {.value = 29, .label = "shared"};
  test_node_t root = {
      .value = 17, .label = "root", .left = &shared, .right = &shared};
  shared.left = &root;

  core_object_archive_registry_t *registry =
      core_object_archive_registry_new();
  check("register versioned node object codec",
        core_object_archive_register(
            registry, (core_object_archive_type_t){
                          .type_id = TEST_NODE_TYPE,
                          .current_version = 1,
                          .oldest_supported_version = 1,
                          .allocate = node_allocate,
                          .save = node_save,
                          .restore = node_restore,
                          .destroy = node_destroy}));

  core_object_archive_error_t error;
  check("save graph as versioned records",
        core_object_archive_save(path, 65536, TEST_ARCHIVE_ABI, registry,
                                 TEST_NODE_TYPE, &root, &error) &&
            error == CORE_OBJECT_ARCHIVE_OK);
  test_node_t second_root = {.value = 91, .label = "second"};
  check("save a separate library archive",
        core_object_archive_save(path2, 65536, TEST_ARCHIVE_ABI, registry,
                                 TEST_NODE_TYPE, &second_root, &error) &&
            error == CORE_OBJECT_ARCHIVE_OK);
  core_object_archive_t *archive = nullptr;
  check("archive ABI mismatch is rejected before object restoration",
        !core_object_archive_load(path, TEST_ARCHIVE_ABI + 1, registry,
                                  &archive, &error) &&
            error == CORE_OBJECT_ARCHIVE_INCOMPATIBLE && !archive &&
            node_destroy_calls == 0);
  core_object_archive_registry_t *missing_codec =
      core_object_archive_registry_new();
  check("loading requires codecs for referenced object kinds",
        !core_object_archive_load(path, TEST_ARCHIVE_ABI, missing_codec,
                                  &archive, &error) &&
            error == CORE_OBJECT_ARCHIVE_UNKNOWN_TYPE && !archive &&
            !strcmp(core_object_archive_error_string(error),
                    "archive contains an unregistered object type"));
  core_object_archive_registry_free(missing_codec);

  core_object_archive_registry_t *newer_codec =
      core_object_archive_registry_new();
  bool newer_registered = core_object_archive_register(
      newer_codec, (core_object_archive_type_t){
                       .type_id = TEST_NODE_TYPE,
                       .current_version = 2,
                       .oldest_supported_version = 2,
                       .allocate = node_allocate,
                       .save = node_save,
                       .restore = node_restore,
                       .destroy = node_destroy});
  check("unsupported object schema versions fail before restoration",
        newer_registered &&
            !core_object_archive_load(path, TEST_ARCHIVE_ABI, newer_codec,
                                      &archive, &error) &&
            error == CORE_OBJECT_ARCHIVE_INCOMPATIBLE && !archive);
  core_object_archive_registry_free(newer_codec);

  core_object_archive_registry_t *compatible_codec =
      core_object_archive_registry_new();
  bool compatible_registered = core_object_archive_register(
      compatible_codec, (core_object_archive_type_t){
                            .type_id = TEST_NODE_TYPE,
                            .current_version = 2,
                            .oldest_supported_version = 1,
                            .allocate = node_allocate,
                            .save = node_save,
                            .restore = node_restore,
                            .destroy = node_destroy});
  bool older_schema_loaded = compatible_registered &&
      core_object_archive_load(path, TEST_ARCHIVE_ABI, compatible_codec,
                               &archive, &error);
  check("codec supporting an older schema loads the archived version",
        older_schema_loaded && error == CORE_OBJECT_ARCHIVE_OK && archive);
  if (archive)
    core_object_archive_close(archive, nullptr);
  archive = nullptr;
  core_object_archive_registry_free(compatible_codec);
  node_destroy_calls = 0;

  core_object_archive_registry_t *malformed_registry =
      core_object_archive_registry_new();
  core_object_archive_register(
      malformed_registry, (core_object_archive_type_t){
                              .type_id = TEST_NODE_TYPE,
                              .current_version = 1,
                              .oldest_supported_version = 1,
                              .allocate = node_allocate,
                              .save = node_save,
                              .restore = node_restore,
                              .destroy = node_destroy});
  bool payload_size_saved = mutate_first_record(path, 1, 0);
  bool trailing_payload_mutated = payload_size_saved &&
      mutate_first_record(path, 1, saved_first_payload_size + 1);
  check("restore hooks must consume the complete typed payload",
        trailing_payload_mutated &&
            !core_object_archive_load(path, TEST_ARCHIVE_ABI,
                                      malformed_registry, &archive, &error) &&
            error == CORE_OBJECT_ARCHIVE_CALLBACK_FAILED && !archive &&
            node_destroy_calls == 2);
  node_destroy_calls = 0;
  check("restore image after trailing-payload rejection",
        mutate_first_record(path, 1, 0));
  bool bad_id_mutated = mutate_first_record(path, 0, 0);
  check("malformed stable object IDs are rejected before restoration",
        bad_id_mutated &&
            !core_object_archive_load(path, TEST_ARCHIVE_ABI,
                                      malformed_registry, &archive, &error) &&
            error == CORE_OBJECT_ARCHIVE_BAD_FORMAT && !archive &&
            node_destroy_calls == 0);
  bool id_restored = mutate_first_record(path, 1, 0);
  bool bad_payload_mutated =
      id_restored && mutate_first_record(path, 1, SIZE_MAX);
  check("out-of-mapping payload lengths are rejected before restoration",
        bad_payload_mutated &&
            !core_object_archive_load(path, TEST_ARCHIVE_ABI,
                                      malformed_registry, &archive, &error) &&
            error == CORE_OBJECT_ARCHIVE_BAD_FORMAT && !archive &&
            node_destroy_calls == 0);
  check("test image is restored after corruption cases",
        mutate_first_record(path, 1, 0));
  core_object_archive_registry_free(malformed_registry);

  core_object_archive_registry_t *rejecting_codec =
      core_object_archive_registry_new();
  bool reject_registered = core_object_archive_register(
      rejecting_codec, (core_object_archive_type_t){
                           .type_id = TEST_NODE_TYPE,
                           .current_version = 1,
                           .oldest_supported_version = 1,
                           .allocate = node_allocate,
                           .save = node_save,
                           .restore = node_restore,
                           .destroy = node_destroy,
                           .user_data = &reject_node_restore});
  reject_node_restore = true;
  check("codec rejection aborts load and cleans allocated objects",
        reject_registered &&
            !core_object_archive_load(path, TEST_ARCHIVE_ABI,
                                      rejecting_codec, &archive, &error) &&
            error == CORE_OBJECT_ARCHIVE_CALLBACK_FAILED && !archive &&
            node_destroy_calls == 2);
  reject_node_restore = false;
  core_object_archive_registry_free(rejecting_codec);
  node_destroy_calls = 0;

  check("load object graph through registered restore hooks",
        core_object_archive_load(path, TEST_ARCHIVE_ABI, registry, &archive,
                                 &error) &&
            error == CORE_OBJECT_ARCHIVE_OK);
  if (archive) {
    uint64_t root_type = 0;
    test_node_t *loaded = core_object_archive_root(archive, &root_type);
    check("round-trip preserves root type and values",
          root_type == TEST_NODE_TYPE && loaded && loaded->value == 17 &&
              !strcmp(loaded->label, "root"));
    check("object IDs preserve sharing and cycles",
          loaded && loaded->left && loaded->left == loaded->right &&
              loaded->left->value == 29 &&
              !strcmp(loaded->left->label, "shared") &&
              loaded->left->left == loaded);
    core_object_archive_t *second_archive = nullptr;
    bool second_loaded = core_object_archive_load(
        path2, TEST_ARCHIVE_ABI, registry, &second_archive, &error);
    uint64_t second_type = 0;
    test_node_t *second_loaded_root = second_loaded
                                          ? core_object_archive_root(
                                                second_archive, &second_type)
                                          : nullptr;
    check("separately saved libraries can be loaded at the same time",
          second_loaded && error == CORE_OBJECT_ARCHIVE_OK &&
              second_type == TEST_NODE_TYPE && second_loaded_root &&
              second_loaded_root->value == 91);
    if (second_archive)
      core_object_archive_close(second_archive, nullptr);
    check("closing second archive releases only its own objects",
          node_destroy_calls == 1);
    check("close loaded object graph",
          core_object_archive_close(archive, &error) &&
              error == CORE_OBJECT_ARCHIVE_OK && node_destroy_calls == 3);
  }

  core_object_archive_t *reopened_second = nullptr;
  bool reopened_second_ok = core_object_archive_load(
      path2, TEST_ARCHIVE_ABI, registry, &reopened_second, &error);
  uint64_t reopened_second_type = 0;
  test_node_t *reopened_second_root = reopened_second_ok
                                          ? core_object_archive_root(
                                                reopened_second,
                                                &reopened_second_type)
                                          : nullptr;
  check("relocated restoration leaves the saved archive unchanged",
        reopened_second_ok && reopened_second_type == TEST_NODE_TYPE &&
            reopened_second_root && reopened_second_root->value == 91 &&
            !strcmp(reopened_second_root->label, "second"));
  if (reopened_second)
    core_object_archive_close(reopened_second, nullptr);

  core_object_archive_registry_free(registry);
  unlink(path);
  unlink(path2);
  return report("object archive");
}
