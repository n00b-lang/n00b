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

/* Derive an ABI tag for native mapped data from an application/schema tag and
 * the current target's OS, architecture, endianness, pointer/data-model sizes,
 * and fundamental alignments. Change the application tag when persisted C
 * layouts or invariants change. This detects common incompatibilities; it is
 * not a portable serialization format or a substitute for schema versioning. */
uint64_t core_persist_arena_native_abi_tag(uint64_t application_tag);

/* Raw arenas may contain native pointers. Their graph must be self-contained
 * and is only loadable with a compatible ABI and at the recorded virtual
 * address. Reopening never replaces an existing mapping; collisions fail. */
bool core_persist_arena_create(core_string_t path, size_t capacity,
                               uint64_t abi_tag,
                               core_persist_arena_t **out,
                               core_persist_arena_error_t *error);
bool core_persist_arena_open(core_string_t path, uint64_t abi_tag,
                             bool writable,
                             core_persist_arena_t **out,
                             core_persist_arena_error_t *error);
/* Opens a private mapping at any available address. Pointers stored in the
 * image must be rebased with core_persist_arena_rebase_pointer(). Writes are
 * transient and never reach the archive file. */
bool core_persist_arena_open_relocated(
    core_string_t path, uint64_t abi_tag, bool writable,
    core_persist_arena_t **out, core_persist_arena_error_t *error);

void *core_persist_arena_alloc(core_persist_arena_t *arena, size_t size,
                               size_t alignment,
                               core_persist_arena_error_t *error);
/* Scratch allocations are owned by an opened arena, grow without bound, and
 * never touch its mapping or persisted used boundary. They are released on
 * close and count as contained. Suitable for graphs reconstructed during
 * load. */
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
