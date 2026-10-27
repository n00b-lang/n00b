#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/string.h"

typedef struct core_persist_arena core_persist_arena_t;

typedef enum {
  CORE_PERSIST_ARENA_OK,
  CORE_PERSIST_ARENA_INVALID_ARGUMENT,
  CORE_PERSIST_ARENA_IO_ERROR,
  CORE_PERSIST_ARENA_BAD_FORMAT,
  CORE_PERSIST_ARENA_INCOMPATIBLE,
  CORE_PERSIST_ARENA_ADDRESS_UNAVAILABLE,
  CORE_PERSIST_ARENA_READ_ONLY,
  CORE_PERSIST_ARENA_OUT_OF_SPACE,
  CORE_PERSIST_ARENA_BAD_POINTER,
} core_persist_arena_error_t;

uint64_t core_persist_arena_native_abi_tag(uint64_t application_tag);

bool core_persist_arena_create(core_string_t path, size_t capacity,
                               uint64_t abi_tag,
                               core_persist_arena_t **out,
                               core_persist_arena_error_t *error);
bool core_persist_arena_open(core_string_t path, uint64_t abi_tag,
                             bool writable,
                             core_persist_arena_t **out,
                             core_persist_arena_error_t *error);
bool core_persist_arena_open_relocated(
    core_string_t path, uint64_t abi_tag, bool writable,
    core_persist_arena_t **out, core_persist_arena_error_t *error);

void *core_persist_arena_alloc(core_persist_arena_t *arena, size_t size,
                               size_t alignment,
                               core_persist_arena_error_t *error);
void *core_persist_arena_alloc_transient(core_persist_arena_t *arena,
                                         size_t size, size_t alignment,
                                         core_persist_arena_error_t *error);
bool core_persist_arena_set_root(core_persist_arena_t *arena,
                                 const void *root,
                                 core_persist_arena_error_t *error);
void *core_persist_arena_root(const core_persist_arena_t *arena);
bool core_persist_arena_contains(const core_persist_arena_t *arena,
                                 const void *pointer, size_t size);
bool core_persist_arena_rebase_pointer(const core_persist_arena_t *arena,
                                       const void *saved_pointer, size_t size,
                                       const void **out);
void *core_persist_arena_base(const core_persist_arena_t *arena);
size_t core_persist_arena_capacity(const core_persist_arena_t *arena);
size_t core_persist_arena_used(const core_persist_arena_t *arena);
bool core_persist_arena_sync(core_persist_arena_t *arena,
                             core_persist_arena_error_t *error);
bool core_persist_arena_close(core_persist_arena_t *arena,
                              core_persist_arena_error_t *error);
