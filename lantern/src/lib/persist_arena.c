#define _XOPEN_SOURCE 700

#include "lib/persist_arena.h"

#include <errno.h>
#include <fcntl.h>
#include <stdalign.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib/alloc.h"

#define CORE_PERSIST_ARENA_MAGIC UINT64_C(0x4c414e544152454e)
#define CORE_PERSIST_ARENA_VERSION 1
#define CORE_PERSIST_ARENA_ENDIAN UINT32_C(0x01020304)
#define CORE_PERSIST_ARENA_NO_ROOT UINT64_MAX

#if defined(__x86_64__) || defined(_M_X64)
#define CORE_PERSIST_ARCH_ID UINT64_C(1)
#elif defined(__aarch64__) || defined(_M_ARM64)
#define CORE_PERSIST_ARCH_ID UINT64_C(2)
#elif defined(__i386__) || defined(_M_IX86)
#define CORE_PERSIST_ARCH_ID UINT64_C(3)
#elif defined(__arm__) || defined(_M_ARM)
#define CORE_PERSIST_ARCH_ID UINT64_C(4)
#else
#define CORE_PERSIST_ARCH_ID UINT64_C(255)
#endif

#if defined(__APPLE__)
#define CORE_PERSIST_OS_ID UINT64_C(1)
#elif defined(_WIN32)
#define CORE_PERSIST_OS_ID UINT64_C(2)
#elif defined(__linux__)
#define CORE_PERSIST_OS_ID UINT64_C(3)
#elif defined(__FreeBSD__)
#define CORE_PERSIST_OS_ID UINT64_C(4)
#else
#define CORE_PERSIST_OS_ID UINT64_C(255)
#endif

typedef struct {
  uint64_t mapping_base;
  uint64_t magic;
  uint32_t version;
  uint32_t endian;
  uint32_t pointer_size;
  uint32_t header_size;
  uint64_t abi_tag;
  uint64_t mapping_size;
  uint64_t used;
  uint64_t root_offset;
} core_persist_arena_header_t;

static uint64_t abi_mix(uint64_t hash, uint64_t value) {
  for (unsigned i = 0; i < 8; i++) {
    hash ^= (uint8_t)(value >> (i * 8));
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

uint64_t core_persist_arena_native_abi_tag(uint64_t application_tag) {
  const uint16_t endian_probe = 1;
  uint64_t hash = UINT64_C(14695981039346656037);
  const uint64_t values[] = {
      application_tag,
      CORE_PERSIST_ARCH_ID,
      CORE_PERSIST_OS_ID,
      sizeof(void *),
      sizeof(void (*)(void)),
      sizeof(size_t),
      sizeof(long),
      sizeof(wchar_t),
      sizeof(long double),
      alignof(max_align_t),
      alignof(core_persist_arena_header_t),
      sizeof(core_persist_arena_header_t),
      *(const uint8_t *)&endian_probe,
  };
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    hash = abi_mix(hash, values[i]);
  return hash;
}

struct core_persist_arena {
  void *mapping;
  size_t mapping_size;
  int fd;
  bool writable;
  bool private_mapping;
  uint64_t saved_base;
  core_persist_arena_header_t *header;
  size_t transient_used;
};

static size_t payload_offset(void) {
  size_t align = alignof(max_align_t);
  return (sizeof(core_persist_arena_header_t) + align - 1) & ~(align - 1);
}

static void set_error(core_persist_arena_error_t *out,
                      core_persist_arena_error_t error) {
  if (out)
    *out = error;
}

static bool fail(core_persist_arena_error_t *out,
                 core_persist_arena_error_t error) {
  set_error(out, error);
  return false;
}

static core_persist_arena_t *wrap_mapping(int fd, void *mapping,
                                          size_t mapping_size, bool writable,
                                          bool private_mapping,
                                          core_persist_arena_header_t *header) {
  core_persist_arena_t *arena = core_alloc(core_persist_arena_t);
  arena->fd = fd;
  arena->mapping = mapping;
  arena->mapping_size = mapping_size;
  arena->writable = writable;
  arena->private_mapping = private_mapping;
  arena->saved_base = header->mapping_base;
  arena->header = header;
  return arena;
}

bool core_persist_arena_create(core_string_t path, size_t capacity,
                               uint64_t abi_tag,
                               core_persist_arena_t **out,
                               core_persist_arena_error_t *error) {
  if (out)
    *out = nullptr;
  if (!path.data || !path.u8_bytes || !out || !capacity ||
      capacity > SIZE_MAX - payload_offset())
    return fail(error, CORE_PERSIST_ARENA_INVALID_ARGUMENT);

  size_t mapping_size = payload_offset() + capacity;
  if ((uintmax_t)mapping_size > (uintmax_t)INT64_MAX)
    return fail(error, CORE_PERSIST_ARENA_INVALID_ARGUMENT);
  char *cpath = core_string_to_cstr(path);
  int fd = open(cpath, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    core_free(cpath);
    return fail(error, CORE_PERSIST_ARENA_IO_ERROR);
  }
  if (ftruncate(fd, (off_t)mapping_size) != 0) {
    close(fd);
    unlink(cpath);
    core_free(cpath);
    return fail(error, CORE_PERSIST_ARENA_IO_ERROR);
  }

  void *mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    close(fd);
    unlink(cpath);
    core_free(cpath);
    return fail(error, CORE_PERSIST_ARENA_IO_ERROR);
  }
  core_free(cpath);

  core_persist_arena_header_t *header = mapping;
  *header = (core_persist_arena_header_t){
      .mapping_base = (uint64_t)(uintptr_t)mapping,
      .magic = CORE_PERSIST_ARENA_MAGIC,
      .version = CORE_PERSIST_ARENA_VERSION,
      .endian = CORE_PERSIST_ARENA_ENDIAN,
      .pointer_size = sizeof(void *),
      .header_size = sizeof(*header),
      .abi_tag = abi_tag,
      .mapping_size = mapping_size,
      .root_offset = CORE_PERSIST_ARENA_NO_ROOT,
  };

  core_persist_arena_t *arena =
      wrap_mapping(fd, mapping, mapping_size, true, false, header);
  *out = arena;
  set_error(error, CORE_PERSIST_ARENA_OK);
  return true;
}

static bool read_header(int fd, core_persist_arena_header_t *header) {
  size_t done = 0;
  while (done < sizeof(*header)) {
    ssize_t count = pread(fd, (char *)header + done, sizeof(*header) - done,
                          (off_t)done);
    if (count <= 0)
      return false;
    done += (size_t)count;
  }
  return true;
}

static bool open_arena(core_string_t path, uint64_t abi_tag, bool writable,
                       bool relocated, core_persist_arena_t **out,
                       core_persist_arena_error_t *error) {
  if (out)
    *out = nullptr;
  if (!path.data || !path.u8_bytes || !out)
    return fail(error, CORE_PERSIST_ARENA_INVALID_ARGUMENT);

  char *cpath = core_string_to_cstr(path);
  int fd = open(cpath, writable ? O_RDWR : O_RDONLY);
  core_free(cpath);
  if (fd < 0)
    return fail(error, CORE_PERSIST_ARENA_IO_ERROR);
  struct stat statbuf;
  core_persist_arena_header_t header;
  if (fstat(fd, &statbuf) != 0 ||
      (uintmax_t)statbuf.st_size < sizeof(header) ||
      !read_header(fd, &header)) {
    close(fd);
    return fail(error, CORE_PERSIST_ARENA_BAD_FORMAT);
  }
  if (header.magic != CORE_PERSIST_ARENA_MAGIC ||
      header.version != CORE_PERSIST_ARENA_VERSION ||
      header.header_size != sizeof(header) ||
      header.mapping_size != (uint64_t)statbuf.st_size ||
      header.mapping_size > SIZE_MAX ||
      header.mapping_size <= payload_offset()) {
    close(fd);
    return fail(error, CORE_PERSIST_ARENA_BAD_FORMAT);
  }
  if (header.endian != CORE_PERSIST_ARENA_ENDIAN ||
      header.pointer_size != sizeof(void *) ||
      header.abi_tag != abi_tag) {
    close(fd);
    return fail(error, CORE_PERSIST_ARENA_INCOMPATIBLE);
  }
  if (header.mapping_base > UINTPTR_MAX ||
      header.mapping_size > UINTPTR_MAX - header.mapping_base ||
      header.used > header.mapping_size - payload_offset() ||
      (header.root_offset != CORE_PERSIST_ARENA_NO_ROOT &&
       header.root_offset >= header.used)) {
    close(fd);
    return fail(error, CORE_PERSIST_ARENA_BAD_FORMAT);
  }

  int protection = PROT_READ | (writable ? PROT_WRITE : 0);
  void *requested = relocated ? nullptr : (void *)(uintptr_t)header.mapping_base;
  int flags = relocated ? MAP_PRIVATE : MAP_SHARED;
  void *mapping = mmap(requested, (size_t)header.mapping_size, protection,
                       flags, fd, 0);
  if (mapping == MAP_FAILED) {
    close(fd);
    return fail(error, CORE_PERSIST_ARENA_ADDRESS_UNAVAILABLE);
  }
  if (!relocated && mapping != requested) {
    munmap(mapping, (size_t)header.mapping_size);
    close(fd);
    return fail(error, CORE_PERSIST_ARENA_ADDRESS_UNAVAILABLE);
  }

  core_persist_arena_t *arena = wrap_mapping(
      fd, mapping, (size_t)header.mapping_size, writable, relocated, mapping);
  *out = arena;
  set_error(error, CORE_PERSIST_ARENA_OK);
  return true;
}

bool core_persist_arena_open(core_string_t path, uint64_t abi_tag,
                             bool writable, core_persist_arena_t **out,
                             core_persist_arena_error_t *error) {
  return open_arena(path, abi_tag, writable, false, out, error);
}

bool core_persist_arena_open_relocated(
    core_string_t path, uint64_t abi_tag, bool writable,
    core_persist_arena_t **out, core_persist_arena_error_t *error) {
  return open_arena(path, abi_tag, writable, true, out, error);
}

void *core_persist_arena_alloc(core_persist_arena_t *arena, size_t size,
                               size_t alignment,
                               core_persist_arena_error_t *error) {
  if (!arena || !size || !alignment || (alignment & (alignment - 1))) {
    set_error(error, CORE_PERSIST_ARENA_INVALID_ARGUMENT);
    return nullptr;
  }
  if (!arena->writable) {
    set_error(error, CORE_PERSIST_ARENA_READ_ONLY);
    return nullptr;
  }
  uintptr_t payload = (uintptr_t)arena->mapping + payload_offset();
  if (payload > UINTPTR_MAX - arena->header->used) {
    set_error(error, CORE_PERSIST_ARENA_OUT_OF_SPACE);
    return nullptr;
  }
  uintptr_t current = payload + (uintptr_t)arena->header->used;
  if (current > UINTPTR_MAX - (alignment - 1)) {
    set_error(error, CORE_PERSIST_ARENA_OUT_OF_SPACE);
    return nullptr;
  }
  uintptr_t aligned = (current + alignment - 1) & ~(uintptr_t)(alignment - 1);
  size_t padding = (size_t)(aligned - current);
  size_t capacity = arena->mapping_size - payload_offset();
  if (padding > capacity - (size_t)arena->header->used ||
      size > capacity - (size_t)arena->header->used - padding) {
    set_error(error, CORE_PERSIST_ARENA_OUT_OF_SPACE);
    return nullptr;
  }
  arena->header->used += padding + size;
  void *result = (void *)aligned;
  memset(result, 0, size);
  set_error(error, CORE_PERSIST_ARENA_OK);
  return result;
}

void *core_persist_arena_alloc_transient(core_persist_arena_t *arena,
                                         size_t size, size_t alignment,
                                         core_persist_arena_error_t *error) {
  if (!arena || !size || !alignment || (alignment & (alignment - 1))) {
    set_error(error, CORE_PERSIST_ARENA_INVALID_ARGUMENT);
    return nullptr;
  }
  if (!arena->writable) {
    set_error(error, CORE_PERSIST_ARENA_READ_ONLY);
    return nullptr;
  }
  size_t capacity = arena->mapping_size - payload_offset();
  if (arena->header->used > capacity ||
      arena->transient_used > capacity - arena->header->used) {
    set_error(error, CORE_PERSIST_ARENA_OUT_OF_SPACE);
    return nullptr;
  }
  size_t current_offset = (size_t)arena->header->used +
                          arena->transient_used;
  uintptr_t payload = (uintptr_t)arena->mapping + payload_offset();
  if (payload > UINTPTR_MAX - current_offset) {
    set_error(error, CORE_PERSIST_ARENA_OUT_OF_SPACE);
    return nullptr;
  }
  uintptr_t address = payload + current_offset;
  if (address > UINTPTR_MAX - (alignment - 1)) {
    set_error(error, CORE_PERSIST_ARENA_OUT_OF_SPACE);
    return nullptr;
  }
  uintptr_t aligned_address =
      (address + alignment - 1) & ~(uintptr_t)(alignment - 1);
  size_t padding = (size_t)(aligned_address - address);
  if (padding > capacity - current_offset ||
      size > capacity - current_offset - padding) {
    set_error(error, CORE_PERSIST_ARENA_OUT_OF_SPACE);
    return nullptr;
  }
  arena->transient_used += padding + size;
  void *result = (void *)aligned_address;
  memset(result, 0, size);
  set_error(error, CORE_PERSIST_ARENA_OK);
  return result;
}

bool core_persist_arena_set_root(core_persist_arena_t *arena,
                                 const void *root,
                                 core_persist_arena_error_t *error) {
  if (!arena || !root)
    return fail(error, CORE_PERSIST_ARENA_INVALID_ARGUMENT);
  if (!arena->writable)
    return fail(error, CORE_PERSIST_ARENA_READ_ONLY);
  uintptr_t start = (uintptr_t)arena->mapping + payload_offset();
  uintptr_t address = (uintptr_t)root;
  if (address < start || address - start >= arena->header->used)
    return fail(error, CORE_PERSIST_ARENA_BAD_POINTER);
  arena->header->root_offset = address - start;
  set_error(error, CORE_PERSIST_ARENA_OK);
  return true;
}

void *core_persist_arena_root(const core_persist_arena_t *arena) {
  if (!arena || arena->header->root_offset == CORE_PERSIST_ARENA_NO_ROOT)
    return nullptr;
  return (char *)arena->mapping + payload_offset() +
         arena->header->root_offset;
}

bool core_persist_arena_contains(const core_persist_arena_t *arena,
                                 const void *pointer, size_t size) {
  if (!arena || !pointer)
    return false;
  uintptr_t start = (uintptr_t)arena->mapping + payload_offset();
  uintptr_t address = (uintptr_t)pointer;
  size_t used = (size_t)arena->header->used + arena->transient_used;
  if (address < start || address - start > used)
    return false;
  return size <= used - (size_t)(address - start);
}

bool core_persist_arena_rebase_pointer(const core_persist_arena_t *arena,
                                       const void *saved_pointer, size_t size,
                                       const void **out) {
  if (out) *out = nullptr;
  if (!arena || !saved_pointer || !out)
    return false;
  uintptr_t saved = (uintptr_t)saved_pointer;
  uintptr_t payload = arena->saved_base + payload_offset();
  size_t used = (size_t)arena->header->used;
  if (saved < payload || saved - payload > used ||
      size > used - (size_t)(saved - payload))
    return false;
  *out = (char *)arena->mapping + (size_t)(saved - arena->saved_base);
  return true;
}

void *core_persist_arena_base(const core_persist_arena_t *arena) {
  return arena ? arena->mapping : nullptr;
}

size_t core_persist_arena_capacity(const core_persist_arena_t *arena) {
  return arena ? arena->mapping_size - payload_offset() : 0;
}

size_t core_persist_arena_used(const core_persist_arena_t *arena) {
  return arena ? (size_t)arena->header->used : 0;
}

bool core_persist_arena_sync(core_persist_arena_t *arena,
                             core_persist_arena_error_t *error) {
  if (!arena)
    return fail(error, CORE_PERSIST_ARENA_INVALID_ARGUMENT);
  if (!arena->writable)
    return fail(error, CORE_PERSIST_ARENA_READ_ONLY);
  if (arena->private_mapping) {
    set_error(error, CORE_PERSIST_ARENA_OK);
    return true;
  }
  if (msync(arena->mapping, arena->mapping_size, MS_SYNC) != 0 ||
      fsync(arena->fd) != 0)
    return fail(error, CORE_PERSIST_ARENA_IO_ERROR);
  set_error(error, CORE_PERSIST_ARENA_OK);
  return true;
}

bool core_persist_arena_close(core_persist_arena_t *arena,
                              core_persist_arena_error_t *error) {
  if (!arena)
    return fail(error, CORE_PERSIST_ARENA_INVALID_ARGUMENT);
  bool ok = true;
  if (arena->writable && !arena->private_mapping &&
      (msync(arena->mapping, arena->mapping_size, MS_SYNC) != 0 ||
       fsync(arena->fd) != 0)) {
    set_error(error, CORE_PERSIST_ARENA_IO_ERROR);
    ok = false;
  }
  if (munmap(arena->mapping, arena->mapping_size) != 0) {
    set_error(error, CORE_PERSIST_ARENA_IO_ERROR);
    ok = false;
  }
  if (close(arena->fd) != 0) {
    set_error(error, CORE_PERSIST_ARENA_IO_ERROR);
    ok = false;
  }
  core_free(arena);
  if (ok)
    set_error(error, CORE_PERSIST_ARENA_OK);
  return ok;
}
