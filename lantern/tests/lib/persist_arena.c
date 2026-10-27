#define _XOPEN_SOURCE 700

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "lib/persist_arena.h"
#include "lib/os.h"
#include "lib/buffer.h"

static int temp_file(const char *prefix, core_string_t *path) {
  core_string_t root = core_os_temp_root();
  core_buffer_t stem = {0};
  core_buffer_append_string(&stem, root);
  core_buffer_printf(&stem, "/%s", prefix);
  int fd = core_os_make_unique_file(core_buffer_string(&stem), path);
  core_buffer_release(&stem);
  core_free(root.data);
  return fd;
}

typedef struct persist_node {
  char *name;
  struct persist_node *next;
  uint64_t value;
} persist_node_t;

static void test_native_abi_tag(void) {
  core_string_t target = {0};
  int placeholder = temp_file("lantern-persist-abi-", &target);
  char *path = core_string_to_cstr(target);
  check("temporary ABI-tag path is available", placeholder >= 0);
  if (placeholder < 0) return;
  close(placeholder);
  unlink(path);

  uint64_t schema = UINT64_C(0x746573742d616269);
  uint64_t tag = core_persist_arena_native_abi_tag(schema);
  check("native ABI tags incorporate the application schema",
        tag != 0 && tag != core_persist_arena_native_abi_tag(schema + 1));

  core_persist_arena_t *arena = nullptr;
  core_persist_arena_error_t error;
  bool created = core_persist_arena_create(target, 256, tag, &arena, &error);
  check("create arena with a derived native ABI tag", created && arena);
  if (arena) core_persist_arena_close(arena, nullptr);
  arena = nullptr;
  check("native ABI tag reopens on the same target",
        core_persist_arena_open(target, tag, false, &arena, &error) && arena);
  if (arena) core_persist_arena_close(arena, nullptr);
  arena = nullptr;
  check("different application schema tag is rejected",
        !core_persist_arena_open(
            target, core_persist_arena_native_abi_tag(schema + 1), false,
            &arena, &error) && error == CORE_PERSIST_ARENA_INCOMPATIBLE &&
            !arena);
  unlink(path);
}

static void test_round_trip_and_address_collision(void) {
  core_string_t target = {0};
  int placeholder = temp_file("lantern-persist-arena-", &target);
  char *path = core_string_to_cstr(target);
  check("temporary snapshot path is available", placeholder >= 0);
  if (placeholder < 0)
    return;
  close(placeholder);
  unlink(path);

  core_persist_arena_error_t error;
  core_persist_arena_t *arena = nullptr;
  check("create file-backed persistent arena",
        core_persist_arena_create(target, 16384, UINT64_C(0x746573742d7631),
                                  &arena, &error) &&
            error == CORE_PERSIST_ARENA_OK);
  if (!arena)
    return;

  persist_node_t *root = core_persist_arena_alloc(
      arena, sizeof(*root), alignof(persist_node_t), &error);
  persist_node_t *next = core_persist_arena_alloc(
      arena, sizeof(*next), alignof(persist_node_t), &error);
  char *root_name = core_persist_arena_alloc(arena, 6, alignof(char), &error);
  char *next_name = core_persist_arena_alloc(arena, 5, alignof(char), &error);
  if (!root || !next || !root_name || !next_name) {
    check("allocate self-contained pointer graph", false);
    core_persist_arena_close(arena, nullptr);
    unlink(path);
    return;
  }
  memcpy(root_name, "first", 6);
  memcpy(next_name, "last", 5);
  *root = (persist_node_t){.name = root_name, .next = next, .value = 17};
  *next = (persist_node_t){.name = next_name, .value = 29};

  void *saved_base = core_persist_arena_base(arena);
  check("set the root and flush the pointer graph",
        core_persist_arena_set_root(arena, root, &error) &&
            core_persist_arena_sync(arena, &error) &&
            core_persist_arena_contains(arena, root, sizeof(*root)) &&
            !core_persist_arena_contains(arena, root, 16384));

  core_persist_arena_t *collision = nullptr;
  check("an occupied saved address is rejected without replacing the mapping",
        !core_persist_arena_open(target, UINT64_C(0x746573742d7631), false,
                                 &collision, &error) &&
            error == CORE_PERSIST_ARENA_ADDRESS_UNAVAILABLE && collision ==
                nullptr && root->value == 17 && root->next->value == 29);

  check("close the original mapping", core_persist_arena_close(arena, &error));
  arena = nullptr;
  check("reopen the arena at its recorded address",
        core_persist_arena_open(target, UINT64_C(0x746573742d7631), false,
                                &arena, &error) &&
            error == CORE_PERSIST_ARENA_OK &&
            core_persist_arena_base(arena) == saved_base);
  size_t saved_used = arena ? core_persist_arena_used(arena) : 0;
  if (arena) {
    persist_node_t *loaded = core_persist_arena_root(arena);
    check("absolute pointers and root survive remapping at the saved base",
          loaded && loaded->value == 17 && !memcmp(loaded->name, "first", 6) &&
              loaded->next && loaded->next->value == 29 &&
              !memcmp(loaded->next->name, "last", 5));
    check("read-only mappings reject allocation",
          !core_persist_arena_alloc(arena, 8, alignof(uint64_t), &error) &&
              error == CORE_PERSIST_ARENA_READ_ONLY);
    check("close the read-only mapping",
          core_persist_arena_close(arena, &error));
    arena = nullptr;
  }

  check("open a writable mapping for transient restoration data",
        core_persist_arena_open(target, UINT64_C(0x746573742d7631), true,
                                &arena, &error));
  if (arena) {
    void *raw_base = core_persist_arena_base(arena);
    core_persist_arena_t *relocated = nullptr;
    bool relocated_open = core_persist_arena_open_relocated(
        target, UINT64_C(0x746573742d7631), true, &relocated, &error);
    persist_node_t *relocated_root = relocated
                                         ? core_persist_arena_root(relocated)
                                         : nullptr;
    const void *relocated_name = nullptr;
    bool name_rebased = relocated_root && core_persist_arena_rebase_pointer(
        relocated, relocated_root->name, 6, &relocated_name);
    check("relocated private mapping rebases bounded persisted pointers",
          relocated_open && relocated &&
              core_persist_arena_base(relocated) != raw_base &&
              relocated_root && relocated_root->value == 17 && name_rebased &&
              !memcmp(relocated_name, "first", 6) &&
              !core_persist_arena_rebase_pointer(
                  relocated, (const void *)UINTPTR_MAX, 1, &relocated_name));
    if (relocated)
      check("close relocated private mapping",
            core_persist_arena_close(relocated, &error));
    void *scratch = core_persist_arena_alloc_transient(
        arena, 16, alignof(uint64_t), &error);
    check("transient allocation is addressable but not persisted as used",
          scratch && core_persist_arena_contains(arena, scratch, 16) &&
              core_persist_arena_used(arena) == saved_used);
    check("close the writable restored mapping",
          core_persist_arena_close(arena, &error));
    arena = nullptr;
  }
  check("reopen preserves the serialized allocation boundary",
        core_persist_arena_open(target, UINT64_C(0x746573742d7631), false,
                                &arena, &error) && arena &&
            core_persist_arena_used(arena) == saved_used);
  if (arena)
    core_persist_arena_close(arena, nullptr);

  check("an incompatible object ABI is rejected before mapping",
        !core_persist_arena_open(target, UINT64_C(0x7465732d7632), false,
                                 &arena, &error) &&
            error == CORE_PERSIST_ARENA_INCOMPATIBLE && arena == nullptr);
  unlink(path);
}

static void test_malformed_file(void) {
  core_string_t target = {0};
  int fd = temp_file("lantern-persist-bad-", &target);
  char *path = core_string_to_cstr(target);
  check("temporary malformed-file path is available", fd >= 0);
  if (fd < 0)
    return;
  close(fd);

  core_persist_arena_t *arena = nullptr;
  core_persist_arena_error_t error;
  check("truncated snapshots fail format validation",
        !core_persist_arena_open(target, 0, false, &arena, &error) &&
            error == CORE_PERSIST_ARENA_BAD_FORMAT && arena == nullptr);
  unlink(path);
}

int main(void) {
  test_native_abi_tag();
  test_round_trip_and_address_collision();
  test_malformed_file();
  return report("persistent arena");
}
