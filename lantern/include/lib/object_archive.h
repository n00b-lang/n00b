#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/persist_arena.h"
#include "lib/string.h"

typedef struct core_object_archive_registry core_object_archive_registry_t;
typedef struct core_object_archive core_object_archive_t;
typedef struct core_object_writer core_object_writer_t;
typedef struct core_object_reader core_object_reader_t;

typedef enum {
  CORE_OBJECT_ARCHIVE_OK,
  CORE_OBJECT_ARCHIVE_INVALID_ARGUMENT,
  CORE_OBJECT_ARCHIVE_IO_ERROR,
  CORE_OBJECT_ARCHIVE_BAD_FORMAT,
  CORE_OBJECT_ARCHIVE_INCOMPATIBLE,
  CORE_OBJECT_ARCHIVE_ADDRESS_UNAVAILABLE,
  CORE_OBJECT_ARCHIVE_UNKNOWN_TYPE,
  CORE_OBJECT_ARCHIVE_CALLBACK_FAILED,
  CORE_OBJECT_ARCHIVE_OUT_OF_SPACE,
} core_object_archive_error_t;
core_string_t core_object_archive_error_string(
    core_object_archive_error_t error);

typedef void *(*core_object_allocate_fn)(core_persist_arena_t *arena,
                                         uint32_t schema_version,
                                         core_object_reader_t *payload,
                                         void *user_data);
typedef bool (*core_object_save_fn)(core_object_writer_t *writer,
                                    const void *object, void *user_data);
typedef bool (*core_object_restore_fn)(core_object_reader_t *reader,
                                       void *object, void *user_data);
typedef bool (*core_object_finalize_fn)(void *object, void *user_data);
typedef void (*core_object_destroy_fn)(void *object, void *user_data);

typedef struct {
  uint64_t type_id;
  uint32_t version;
  core_object_allocate_fn allocate;
  core_object_save_fn save;
  core_object_restore_fn restore;
  core_object_finalize_fn finalize;
  core_object_destroy_fn destroy;
  void *user_data;
} core_object_archive_type_t;

/* Register codecs before use. Do not mutate a registry while save/load calls
 * are using it; concurrent calls may share a fully populated registry. */
core_object_archive_registry_t *core_object_archive_registry_new(void);
void core_object_archive_registry_free(core_object_archive_registry_t *registry);
bool core_object_archive_register(core_object_archive_registry_t *registry,
                                  core_object_archive_type_t type);

bool core_object_writer_bytes(core_object_writer_t *writer, const void *data,
                              size_t size);
bool core_object_writer_u32(core_object_writer_t *writer, uint32_t value);
bool core_object_writer_u64(core_object_writer_t *writer, uint64_t value);
bool core_object_writer_string(core_object_writer_t *writer,
                               core_string_t value);
/* A null reference may use type_id 0; non-null references require a registered
 * nonzero type ID. */
bool core_object_writer_ref(core_object_writer_t *writer, uint64_t type_id,
                            const void *object);

bool core_object_reader_bytes(core_object_reader_t *reader, void *out,
                              size_t size);
void *core_object_reader_alloc(core_object_reader_t *reader, size_t size,
                               size_t alignment);
bool core_object_reader_u32(core_object_reader_t *reader, uint32_t *out);
bool core_object_reader_u64(core_object_reader_t *reader, uint64_t *out);
bool core_object_reader_string(core_object_reader_t *reader,
                               core_string_t *out);
/* A nonzero type ID requires an exact match. Zero accepts any registered type;
 * null references are accepted for either form. */
bool core_object_reader_ref(core_object_reader_t *reader, uint64_t type_id,
                            void **out);

/* Every reachable object kind must have a registered codec. The mapped image
 * uses stable type/version records and object IDs; codecs own payload schemas.
 * The image's ABI tag still applies because its record layout is native.
 * Allocate hooks use core_persist_arena_alloc_transient(); restored objects
 * then live in the archive's arena without changing its persisted boundary.
 * All loaded object pointers are borrowed from the archive and expire on close;
 * optional finalize hooks run after every object's references are restored;
 * destroy hooks release codec-owned external resources on failure or close. */
bool core_object_archive_save(core_string_t path, size_t capacity,
                             uint64_t abi_tag,
                             core_object_archive_registry_t *registry,
                             uint64_t root_type_id, const void *root,
                             core_object_archive_error_t *error);
bool core_object_archive_load(core_string_t path, uint64_t abi_tag,
                             core_object_archive_registry_t *registry,
                             core_object_archive_t **out,
                             core_object_archive_error_t *error);
void *core_object_archive_root(const core_object_archive_t *archive,
                               uint64_t *type_id);
bool core_object_archive_close(core_object_archive_t *archive,
                               core_object_archive_error_t *error);
