#include "lib/os.h"
#include "llvm_native_cache.h"

#include "cache_location.h"

#include "lib/alloc.h"
#include "lib/hash.h"
#include "lib/string.h"

#include <llvm-c/Core.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

struct n00bb_native_cache_t {
    core_string_t directory;
    core_uint128_t compiler_hash;
};

typedef struct {
    uint64_t magic;
    uint64_t key_low;
    uint64_t key_high;
    uint64_t object_bytes;
    uint64_t object_hash_low;
    uint64_t object_hash_high;
} cache_header_t;

static const uint64_t CACHE_MAGIC = UINT64_C(0x4e30304243414348);

static bool
cache_compiler_hash(core_string_t executable_path, core_uint128_t *out)
{
    char *image = nullptr;
    size_t size = 0;
    if (!core_os_read_file(executable_path, &image, &size)) return false;
    *out = core_xxh3_128bits_raw(image, size);
    core_free(image);
    return true;
}

static core_buffer_t
cache_file_path(const n00bb_native_cache_t *cache, core_uint128_t key)
{
    core_buffer_t path = {0};
    core_buffer_printf(&path, "%.*s/%016llx%016llx.o",
        (int)cache->directory.u8_bytes, cache->directory.data,
        (unsigned long long)(key >> 64), (unsigned long long)key);
    return path;
}

core_string_t
n00bb_native_cache_root(void)
{
    core_string_t directory = n00bb_cache_directory(
        CORE_STRING_STATIC("native"));
    if (directory.data && n00bb_make_directories(directory)) return directory;
    core_free(directory.data);
    return (core_string_t){};
}

n00bb_native_cache_t *
n00bb_native_cache_open(core_string_t executable_path)
{
    n00bb_native_cache_t *cache = core_alloc(n00bb_native_cache_t);
    if (cache) cache->directory = n00bb_native_cache_root();
    if (!cache || !cache->directory.data
        || !cache_compiler_hash(executable_path, &cache->compiler_hash)) {
        n00bb_native_cache_close(cache);
        return nullptr;
    }
    return cache;
}

void
n00bb_native_cache_close(n00bb_native_cache_t *cache)
{
    if (!cache) return;
    core_free(cache->directory.data);
    core_free(cache);
}

core_uint128_t
n00bb_native_cache_key(const n00bb_native_cache_t *cache,
    const void *bitcode, size_t bitcode_bytes, core_string_t target_triple,
    core_string_t cpu, core_string_t features, bool debug_enabled,
    bool frame_maps)
{
    if (!cache || !bitcode || !target_triple.data) return 0;
    unsigned version[3] = {0};
    LLVMGetVersion(&version[0], &version[1], &version[2]);
    uint8_t flags[] = {debug_enabled, frame_maps};
    core_buffer_t input = {0};
    core_buffer_append(&input, (const char *)&cache->compiler_hash,
        sizeof cache->compiler_hash);
    core_buffer_append(&input, (const char *)version, sizeof version);
    core_buffer_append(&input, (const char *)flags, sizeof flags);
    core_buffer_append_string(&input, target_triple);
    core_buffer_append(&input, "", 1);
    core_buffer_append_string(&input, cpu);
    core_buffer_append(&input, "", 1);
    core_buffer_append_string(&input, features);
    core_buffer_append(&input, "", 1);
    core_buffer_append(&input, bitcode, bitcode_bytes);
    core_uint128_t hash = core_xxh3_128bits_raw(input.data, input.byte_len);
    core_buffer_release(&input);
    return hash;
}

bool
n00bb_native_cache_read(const n00bb_native_cache_t *cache,
    core_uint128_t key, core_buffer_t *object)
{
    if (!cache || !key || !object) return false;
    core_buffer_t path = cache_file_path(cache, key);
    char *bytes = nullptr;
    size_t size = 0;
    bool ok = core_os_read_file(core_buffer_string(&path), &bytes, &size)
        && size > sizeof(cache_header_t);
    core_buffer_release(&path);
    cache_header_t header = {0};
    if (ok) memcpy(&header, bytes, sizeof header);
    ok = ok && header.magic == CACHE_MAGIC
        && header.key_low == (uint64_t)key
        && header.key_high == (uint64_t)(key >> 64)
        && header.object_bytes == size - sizeof header;
    if (ok) {
        core_uint128_t hash = core_xxh3_128bits_raw(bytes + sizeof header,
            (size_t)header.object_bytes);
        ok = (uint64_t)hash == header.object_hash_low
            && (uint64_t)(hash >> 64) == header.object_hash_high;
    }
    if (ok) core_buffer_append(object, bytes + sizeof header,
                               (size_t)header.object_bytes);
    core_free(bytes);
    return ok;
}

void
n00bb_native_cache_write(const n00bb_native_cache_t *cache,
    core_uint128_t key, const void *object, size_t object_bytes)
{
    if (!cache || !key || !object || !object_bytes) return;
    core_buffer_t path = cache_file_path(cache, key);
    if (!path.data) return;
    core_uint128_t hash = core_xxh3_128bits_raw(object, object_bytes);
    cache_header_t header = {
        .magic = CACHE_MAGIC,
        .key_low = (uint64_t)key,
        .key_high = (uint64_t)(key >> 64),
        .object_bytes = object_bytes,
        .object_hash_low = (uint64_t)hash,
        .object_hash_high = (uint64_t)(hash >> 64),
    };
    core_buffer_t entry = {0};
    core_buffer_append(&entry, (const char *)&header, sizeof header);
    core_buffer_append(&entry, object, object_bytes);
    core_os_write_file_atomic(core_buffer_string(&path), entry.data,
                              entry.byte_len);
    core_buffer_release(&entry);
    core_buffer_release(&path);
}
