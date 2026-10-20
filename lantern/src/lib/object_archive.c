#include "lib/object_archive.h"

#include <stdalign.h>
#include <string.h>
#include <unistd.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/byteorder.h"
#include "lib/list.h"

#define CORE_OBJECT_ARCHIVE_MAGIC UINT64_C(0x434f424a41524348)
#define CORE_OBJECT_ARCHIVE_VERSION 1

typedef struct {
  uint64_t id;
  uint64_t type_id;
  uint32_t schema_version;
  uint32_t reserved;
  size_t payload_size;
  const uint8_t *payload;
} archive_record_t;

typedef struct {
  uint64_t magic;
  uint32_t version;
  uint32_t reserved;
  uint64_t root_type_id;
  uint64_t root_object_id;
  size_t record_count;
  const archive_record_t *records;
} archive_image_t;

typedef struct {
  const void *source;
  uint64_t type_id;
  core_object_archive_type_t codec;
  bool has_codec;
  core_buffer_t payload;
  void *restored;
} archive_object_t;
core_list_decl(archive_object_t);
core_list_decl(core_object_archive_type_t);

struct core_object_archive_registry {
  core_list_t(core_object_archive_type_t) codecs;
};

struct core_object_archive {
  core_persist_arena_t *arena;
  core_list_t(archive_object_t) objects;
  uint64_t root_type_id;
  uint64_t root_object_id;
};

struct core_object_writer {
  core_buffer_t *payload;
  core_list_t(archive_object_t) *objects;
  bool ok;
};

struct core_object_reader {
  const uint8_t *data;
  size_t size;
  size_t offset;
  core_persist_arena_t *arena;
  core_list_t(archive_object_t) *objects;
  bool resolve_refs;
  bool ok;
};

core_string_t core_object_archive_error_string(
    core_object_archive_error_t error) {
  switch (error) {
  case CORE_OBJECT_ARCHIVE_OK: return CORE_STRING_STATIC("ok");
  case CORE_OBJECT_ARCHIVE_INVALID_ARGUMENT:
    return CORE_STRING_STATIC("invalid argument");
  case CORE_OBJECT_ARCHIVE_IO_ERROR:
    return CORE_STRING_STATIC("archive I/O failed");
  case CORE_OBJECT_ARCHIVE_BAD_FORMAT:
    return CORE_STRING_STATIC("malformed object archive");
  case CORE_OBJECT_ARCHIVE_INCOMPATIBLE:
    return CORE_STRING_STATIC("archive ABI or object schema is incompatible");
  case CORE_OBJECT_ARCHIVE_ADDRESS_UNAVAILABLE:
    return CORE_STRING_STATIC("archive mapping address is unavailable");
  case CORE_OBJECT_ARCHIVE_UNKNOWN_TYPE:
    return CORE_STRING_STATIC("archive contains an unregistered object type");
  case CORE_OBJECT_ARCHIVE_CALLBACK_FAILED:
    return CORE_STRING_STATIC("object archive codec rejected a record");
  case CORE_OBJECT_ARCHIVE_OUT_OF_SPACE:
    return CORE_STRING_STATIC("object archive mapping has insufficient space");
  }
  return CORE_STRING_STATIC("unknown object archive error");
}

static void set_error(core_object_archive_error_t *out,
                      core_object_archive_error_t error) {
  if (out)
    *out = error;
}

static bool fail(core_object_archive_error_t *out,
                 core_object_archive_error_t error) {
  set_error(out, error);
  return false;
}

static core_object_archive_error_t map_arena_error(
    core_persist_arena_error_t error) {
  switch (error) {
  case CORE_PERSIST_ARENA_OK: return CORE_OBJECT_ARCHIVE_OK;
  case CORE_PERSIST_ARENA_INVALID_ARGUMENT:
  case CORE_PERSIST_ARENA_READ_ONLY:
  case CORE_PERSIST_ARENA_BAD_POINTER:
    return CORE_OBJECT_ARCHIVE_INVALID_ARGUMENT;
  case CORE_PERSIST_ARENA_IO_ERROR: return CORE_OBJECT_ARCHIVE_IO_ERROR;
  case CORE_PERSIST_ARENA_BAD_FORMAT: return CORE_OBJECT_ARCHIVE_BAD_FORMAT;
  case CORE_PERSIST_ARENA_INCOMPATIBLE: return CORE_OBJECT_ARCHIVE_INCOMPATIBLE;
  case CORE_PERSIST_ARENA_ADDRESS_UNAVAILABLE:
    return CORE_OBJECT_ARCHIVE_ADDRESS_UNAVAILABLE;
  case CORE_PERSIST_ARENA_OUT_OF_SPACE: return CORE_OBJECT_ARCHIVE_OUT_OF_SPACE;
  }
  return CORE_OBJECT_ARCHIVE_BAD_FORMAT;
}

static const core_object_archive_type_t *find_codec(
    const core_object_archive_registry_t *registry, uint64_t type_id) {
  if (!registry)
    return nullptr;
  for (size_t i = 0; i < registry->codecs.len; i++)
    if (registry->codecs.data[i].type_id == type_id)
      return &registry->codecs.data[i];
  return nullptr;
}

core_object_archive_registry_t *core_object_archive_registry_new(void) {
  core_object_archive_registry_t *registry =
      core_alloc(core_object_archive_registry_t);
  registry->codecs = core_list_new(core_object_archive_type_t);
  return registry;
}

void core_object_archive_registry_free(core_object_archive_registry_t *registry) {
  if (!registry)
    return;
  core_list_free(registry->codecs);
  core_free(registry);
}

bool core_object_archive_register(core_object_archive_registry_t *registry,
                                  core_object_archive_type_t type) {
  if (!registry || !type.type_id || !type.current_version ||
      !type.oldest_supported_version ||
      type.oldest_supported_version > type.current_version ||
      !type.allocate || !type.save || !type.restore ||
      find_codec(registry, type.type_id))
    return false;
  core_list_push(registry->codecs, type);
  return true;
}

static bool write_raw(core_buffer_t *buffer, const void *data, size_t size) {
  if (size && !data)
    return false;
  core_buffer_append(buffer, data, size);
  return true;
}

bool core_object_writer_bytes(core_object_writer_t *writer, const void *data,
                              size_t size) {
  if (!writer || !writer->ok || !write_raw(writer->payload, data, size))
    return writer ? (writer->ok = false) : false;
  return true;
}

bool core_object_writer_u32(core_object_writer_t *writer, uint32_t value) {
  uint8_t bytes[4];
  core_le_store_u32(bytes, value);
  return core_object_writer_bytes(writer, bytes, sizeof(bytes));
}

bool core_object_writer_u64(core_object_writer_t *writer, uint64_t value) {
  uint8_t bytes[8];
  core_le_store_u64(bytes, value);
  return core_object_writer_bytes(writer, bytes, sizeof(bytes));
}

bool core_object_writer_string(core_object_writer_t *writer,
                               core_string_t value) {
  if (!value.data)
    return core_object_writer_u64(writer, UINT64_MAX);
  return core_object_writer_u64(writer, value.u8_bytes) &&
         core_object_writer_bytes(writer, value.data, value.u8_bytes);
}

static uint64_t object_id_for(core_list_t(archive_object_t) *objects,
                              uint64_t type_id, const void *object,
                              bool *ok) {
  if (!object)
    return 0;
  for (size_t i = 0; i < objects->len; i++) {
    if (objects->data[i].source != object)
      continue;
    if (objects->data[i].type_id != type_id)
      *ok = false;
    return i + 1;
  }
  archive_object_t entry = {.source = object, .type_id = type_id};
  core_list_push(*objects, entry);
  return objects->len;
}

bool core_object_writer_ref(core_object_writer_t *writer, uint64_t type_id,
                            const void *object) {
  if (!writer || !writer->ok || (!type_id && object)) {
    if (writer) writer->ok = false;
    return false;
  }
  if (!type_id)
    return core_object_writer_u64(writer, 0) &&
           core_object_writer_u64(writer, 0);
  bool valid = true;
  uint64_t id = object_id_for(writer->objects, type_id, object, &valid);
  writer->ok = valid && core_object_writer_u64(writer, type_id) &&
               core_object_writer_u64(writer, id);
  return writer->ok;
}

bool core_object_reader_bytes(core_object_reader_t *reader, void *out,
                              size_t size) {
  if (!reader || !reader->ok || (size && !out) ||
      size > reader->size - reader->offset) {
    if (reader) reader->ok = false;
    return false;
  }
  if (size)
    memcpy(out, reader->data + reader->offset, size);
  reader->offset += size;
  return true;
}

void *core_object_reader_alloc(core_object_reader_t *reader, size_t size,
                               size_t alignment) {
  if (!reader || !reader->ok) return nullptr;
  void *result = core_persist_arena_alloc_transient(
      reader->arena, size, alignment, nullptr);
  if (!result) reader->ok = false;
  return result;
}

bool core_object_reader_u32(core_object_reader_t *reader, uint32_t *out) {
  uint8_t bytes[4];
  if (!out || !core_object_reader_bytes(reader, bytes, sizeof(bytes)))
    return false;
  *out = core_le_load_u32(bytes);
  return true;
}

bool core_object_reader_u64(core_object_reader_t *reader, uint64_t *out) {
  uint8_t bytes[8];
  if (!out || !core_object_reader_bytes(reader, bytes, sizeof(bytes)))
    return false;
  *out = core_le_load_u64(bytes);
  return true;
}

bool core_object_reader_string(core_object_reader_t *reader,
                               core_string_t *out) {
  uint64_t size;
  if (!out || !core_object_reader_u64(reader, &size))
    return false;
  if (size == UINT64_MAX) {
    *out = (core_string_t){};
    return true;
  }
  if (size >= SIZE_MAX || size > reader->size - reader->offset) {
    reader->ok = false;
    return false;
  }
  char *string = core_object_reader_alloc(reader, size ? (size_t)size : 1,
                                          alignof(char));
  if (!string || !core_object_reader_bytes(reader, string, (size_t)size))
    return false;
  *out = (core_string_t){.data = string, .u8_bytes = (size_t)size};
  return true;
}

bool core_object_reader_ref(core_object_reader_t *reader, uint64_t type_id,
                            void **out) {
  uint64_t encoded_type, id;
  if (!reader || !out || !reader->resolve_refs ||
      !core_object_reader_u64(reader, &encoded_type) ||
      !core_object_reader_u64(reader, &id) || id > reader->objects->len ||
      (id == 0 ? (encoded_type && type_id && encoded_type != type_id)
               : (!encoded_type || (type_id && encoded_type != type_id)))) {
    if (reader) reader->ok = false;
    return false;
  }
  if (!id) {
    *out = nullptr;
    return true;
  }
  archive_object_t *entry = &reader->objects->data[id - 1];
  if (entry->type_id != encoded_type || !entry->restored) {
    reader->ok = false;
    return false;
  }
  *out = entry->restored;
  return true;
}

static void release_objects(core_list_t(archive_object_t) *objects) {
  for (size_t i = 0; i < objects->len; i++)
    core_buffer_release(&objects->data[i].payload);
  core_list_free(*objects);
}

static void discard_archive(core_object_archive_t *archive) {
  if (!archive)
    return;
  for (size_t i = archive->objects.len; i > 0; i--) {
    archive_object_t *object = &archive->objects.data[i - 1];
    if (object->restored && object->has_codec && object->codec.destroy)
      object->codec.destroy(object->restored, object->codec.user_data);
  }
  release_objects(&archive->objects);
  core_persist_arena_close(archive->arena, nullptr);
  core_free(archive);
}

bool core_object_archive_save(core_string_t path, size_t capacity,
                             uint64_t abi_tag,
                             core_object_archive_registry_t *registry,
                             uint64_t root_type_id, const void *root,
                             core_object_archive_error_t *error) {
  if (!path.data || !registry || !root_type_id || !root)
    return fail(error, CORE_OBJECT_ARCHIVE_INVALID_ARGUMENT);
  core_list_t(archive_object_t) objects = core_list_new(archive_object_t);
  bool valid = true;
  object_id_for(&objects, root_type_id, root, &valid);

  for (size_t i = 0; valid && i < objects.len; i++) {
    archive_object_t *object = &objects.data[i];
    const core_object_archive_type_t *registered_codec =
        find_codec(registry, object->type_id);
    if (!registered_codec) {
      valid = false;
      set_error(error, CORE_OBJECT_ARCHIVE_UNKNOWN_TYPE);
      break;
    }
    object->codec = *registered_codec;
    object->has_codec = true;
    core_buffer_t payload = {0};
    core_object_writer_t writer = {
        .payload = &payload, .objects = &objects, .ok = true};
    const void *source = object->source;
    core_object_archive_type_t codec_copy = object->codec;
    if (!codec_copy.save(&writer, source, codec_copy.user_data) || !writer.ok) {
      core_buffer_release(&payload);
      valid = false;
      set_error(error, CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
      break;
    }
    objects.data[i].payload = payload;
  }
  if (!valid) {
    release_objects(&objects);
    if (error && *error == CORE_OBJECT_ARCHIVE_OK)
      *error = CORE_OBJECT_ARCHIVE_UNKNOWN_TYPE;
    return false;
  }

  if (objects.len > (SIZE_MAX - sizeof(archive_image_t) - 128) /
                        sizeof(archive_record_t)) {
    release_objects(&objects);
    return fail(error, CORE_OBJECT_ARCHIVE_OUT_OF_SPACE);
  }
  if (objects.len > (SIZE_MAX - sizeof(archive_image_t) - 128) /
                        (sizeof(archive_record_t) + 7)) {
    release_objects(&objects);
    return fail(error, CORE_OBJECT_ARCHIVE_OUT_OF_SPACE);
  }
  size_t required = sizeof(archive_image_t) +
                    objects.len * (sizeof(archive_record_t) + 7) + 128;
  for (size_t i = 0; i < objects.len; i++) {
    if (objects.data[i].payload.byte_len > SIZE_MAX - required) {
      release_objects(&objects);
      return fail(error, CORE_OBJECT_ARCHIVE_OUT_OF_SPACE);
    }
    required += objects.data[i].payload.byte_len;
  }
  if (!capacity)
    capacity = required;
  if (capacity < required) {
    release_objects(&objects);
    return fail(error, CORE_OBJECT_ARCHIVE_OUT_OF_SPACE);
  }

  core_persist_arena_error_t arena_error;
  core_persist_arena_t *arena = nullptr;
  if (!core_persist_arena_create(path, capacity, abi_tag, &arena,
                                 &arena_error)) {
    release_objects(&objects);
    return fail(error, map_arena_error(arena_error));
  }
  archive_record_t *records = core_persist_arena_alloc(
      arena, sizeof(*records) * objects.len, alignof(archive_record_t),
      &arena_error);
  bool ok = records != nullptr;
  for (size_t i = 0; ok && i < objects.len; i++) {
    archive_object_t *object = &objects.data[i];
    uint8_t *payload = object->payload.byte_len
        ? core_persist_arena_alloc(arena, object->payload.byte_len,
                                   alignof(uint64_t), &arena_error)
        : nullptr;
    if (object->payload.byte_len && !payload) {
      ok = false;
      break;
    }
    if (payload)
      memcpy(payload, object->payload.data, object->payload.byte_len);
    records[i] = (archive_record_t){
        .id = i + 1,
        .type_id = object->type_id,
        .schema_version = object->codec.current_version,
        .payload_size = object->payload.byte_len,
        .payload = payload,
    };
  }
  archive_image_t *image = nullptr;
  if (ok) {
    image = core_persist_arena_alloc(arena, sizeof(*image),
                                     alignof(archive_image_t), &arena_error);
    ok = image != nullptr;
  }
  if (ok) {
    *image = (archive_image_t){
        .magic = CORE_OBJECT_ARCHIVE_MAGIC,
        .version = CORE_OBJECT_ARCHIVE_VERSION,
        .root_type_id = root_type_id,
        .root_object_id = 1,
        .record_count = objects.len,
        .records = records,
    };
    ok = core_persist_arena_set_root(arena, image, &arena_error) &&
         core_persist_arena_sync(arena, &arena_error);
  }
  core_persist_arena_close(arena, nullptr);
  release_objects(&objects);
  if (!ok) {
    char *cpath = core_string_to_cstr(path);
    unlink(cpath);
    core_free(cpath);
    return fail(error, map_arena_error(arena_error));
  }
  set_error(error, CORE_OBJECT_ARCHIVE_OK);
  return true;
}

static core_object_reader_t make_reader(const archive_record_t *record,
                                        core_persist_arena_t *arena,
                                        core_list_t(archive_object_t) *objects,
                                        bool resolve_refs) {
  return (core_object_reader_t){
      .data = record->payload,
      .size = record->payload_size,
      .arena = arena,
      .objects = objects,
      .resolve_refs = resolve_refs,
      .ok = true,
  };
}

bool core_object_archive_load(core_string_t path, uint64_t abi_tag,
                             core_object_archive_registry_t *registry,
                             core_object_archive_t **out,
                             core_object_archive_error_t *error) {
  if (out) *out = nullptr;
  if (!path.data || !registry || !out)
    return fail(error, CORE_OBJECT_ARCHIVE_INVALID_ARGUMENT);
  core_persist_arena_error_t arena_error;
  core_persist_arena_t *arena = nullptr;
  if (!core_persist_arena_open_relocated(path, abi_tag, true, &arena,
                                         &arena_error))
    return fail(error, map_arena_error(arena_error));

  archive_image_t *image = core_persist_arena_root(arena);
  if (!core_persist_arena_contains(arena, image, sizeof(*image)) ||
      (uintptr_t)image % alignof(archive_image_t) != 0 ||
      image->magic != CORE_OBJECT_ARCHIVE_MAGIC ||
      image->version != CORE_OBJECT_ARCHIVE_VERSION ||
      !image->record_count ||
      image->record_count > SIZE_MAX / sizeof(archive_record_t) ||
      !image->root_object_id || image->root_object_id > image->record_count) {
    core_persist_arena_close(arena, nullptr);
    return fail(error, CORE_OBJECT_ARCHIVE_BAD_FORMAT);
  }
  const void *records_pointer = nullptr;
  bool records_rebased = core_persist_arena_rebase_pointer(
      arena, image->records,
      image->record_count * sizeof(archive_record_t), &records_pointer);
  if (records_rebased)
    image->records = records_pointer;
  if (!records_rebased ||
      !core_persist_arena_contains(arena, image->records,
                                   image->record_count *
                                       sizeof(archive_record_t)) ||
      (uintptr_t)image->records % alignof(archive_record_t) != 0) {
    core_persist_arena_close(arena, nullptr);
    return fail(error, CORE_OBJECT_ARCHIVE_BAD_FORMAT);
  }

  core_object_archive_t *archive = core_alloc(core_object_archive_t);
  archive->arena = arena;
  archive->objects = core_list_new_cap(archive_object_t, image->record_count);
  archive->root_type_id = image->root_type_id;
  archive->root_object_id = image->root_object_id;
  for (size_t i = 0; i < image->record_count; i++) {
    archive_record_t *record = (archive_record_t *)&image->records[i];
    const void *payload_pointer = nullptr;
    if (record->payload_size &&
        core_persist_arena_rebase_pointer(arena, record->payload,
                                          record->payload_size,
                                          &payload_pointer))
      record->payload = payload_pointer;
    const core_object_archive_type_t *codec = find_codec(registry,
                                                          record->type_id);
    if (record->id != i + 1 ||
        (record->payload_size && !core_persist_arena_contains(
            arena, record->payload, record->payload_size))) {
      discard_archive(archive);
      return fail(error, CORE_OBJECT_ARCHIVE_BAD_FORMAT);
    }
    if (!codec) {
      discard_archive(archive);
      return fail(error, CORE_OBJECT_ARCHIVE_UNKNOWN_TYPE);
    }
    if (record->schema_version < codec->oldest_supported_version ||
        record->schema_version > codec->current_version) {
      discard_archive(archive);
      return fail(error, CORE_OBJECT_ARCHIVE_INCOMPATIBLE);
    }
    core_list_push(archive->objects, ((archive_object_t){
        .type_id = record->type_id, .codec = *codec, .has_codec = true}));
  }

  archive_record_t *records = (archive_record_t *)image->records;
  archive_record_t *root_record = &records[image->root_object_id - 1];
  if (root_record->type_id != image->root_type_id) {
    discard_archive(archive);
    return fail(error, CORE_OBJECT_ARCHIVE_BAD_FORMAT);
  }
  for (size_t i = 0; i < archive->objects.len; i++) {
    archive_object_t *object = &archive->objects.data[i];
    core_object_reader_t reader = make_reader(&records[i], arena,
                                               &archive->objects, false);
    object->restored = object->codec.allocate(
        arena, records[i].schema_version, &reader, object->codec.user_data);
    if (!object->restored || !reader.ok ||
        !core_persist_arena_contains(arena, object->restored, 1)) {
      discard_archive(archive);
      return fail(error, CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
    }
  }
  for (size_t i = 0; i < archive->objects.len; i++) {
    archive_object_t *object = &archive->objects.data[i];
    core_object_reader_t reader = make_reader(&records[i], arena,
                                               &archive->objects, true);
    if (!object->codec.restore(&reader, object->restored,
                               object->codec.user_data) || !reader.ok ||
        reader.offset != reader.size) {
      discard_archive(archive);
      return fail(error, CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
    }
  }
  for (size_t i = 0; i < archive->objects.len; i++) {
    archive_object_t *object = &archive->objects.data[i];
    if (object->codec.finalize &&
        !object->codec.finalize(object->restored,
                                object->codec.user_data)) {
      discard_archive(archive);
      return fail(error, CORE_OBJECT_ARCHIVE_CALLBACK_FAILED);
    }
  }
  *out = archive;
  set_error(error, CORE_OBJECT_ARCHIVE_OK);
  return true;
}

void *core_object_archive_root(const core_object_archive_t *archive,
                               uint64_t *type_id) {
  if (!archive || !archive->root_object_id ||
      archive->root_object_id > archive->objects.len)
    return nullptr;
  if (type_id) *type_id = archive->root_type_id;
  return archive->objects.data[archive->root_object_id - 1].restored;
}

bool core_object_archive_close(core_object_archive_t *archive,
                               core_object_archive_error_t *error) {
  if (!archive)
    return fail(error, CORE_OBJECT_ARCHIVE_INVALID_ARGUMENT);
  bool ok = core_persist_arena_sync(archive->arena, nullptr);
  discard_archive(archive);
  set_error(error, ok ? CORE_OBJECT_ARCHIVE_OK
                      : CORE_OBJECT_ARCHIVE_IO_ERROR);
  return ok;
}
