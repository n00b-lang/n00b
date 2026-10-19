#include "llvm_native_cache.h"

#include "lib/alloc.h"
#include "lib/hash.h"
#include "lib/string.h"

#include <llvm-c/Core.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

struct n00bb_native_cache_t {
    core_string_t directory;
    core_uint128_t compiler_hash;
};

#ifndef _WIN32
typedef struct {
    uint64_t magic;
    uint64_t key_low;
    uint64_t key_high;
    uint64_t object_bytes;
    uint64_t object_hash_low;
    uint64_t object_hash_high;
} cache_header_t;

static const uint64_t CACHE_MAGIC = UINT64_C(0x4e30304243414348);
#endif

#ifndef _WIN32
static bool
cache_mkdirs(core_string_t directory)
{
    char *path = core_string_to_cstr(directory);
    if (!path) return false;
    bool ok = true;
    for (char *at = path + 1; *at; at++) {
        if (*at != '/') continue;
        *at = 0;
        if (mkdir(path, 0700) && errno != EEXIST) ok = false;
        *at = '/';
        if (!ok) break;
    }
    if (ok && mkdir(path, 0700) && errno != EEXIST) ok = false;
    struct stat status;
    if (ok) ok = !stat(path, &status) && S_ISDIR(status.st_mode);
    core_free(path);
    return ok;
}

static bool
cache_compiler_hash(core_string_t executable_path, core_uint128_t *out)
{
    char *native_path = core_string_to_cstr(executable_path);
    FILE *file = native_path ? fopen(native_path, "rb") : nullptr;
    core_free(native_path);
    if (!file) return false;
    core_buffer_t image = {0};
    char bytes[64 * 1024];
    size_t length;
    while ((length = fread(bytes, 1, sizeof bytes, file)) != 0)
        core_buffer_append(&image, bytes, length);
    bool ok = !ferror(file);
    if (fclose(file)) ok = false;
    if (ok) *out = core_xxh3_128bits_raw(image.data, image.byte_len);
    core_buffer_release(&image);
    return ok;
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
#endif

core_string_t
n00bb_native_cache_root(void)
{
#ifdef _WIN32
    return (core_string_t){};
#else
    core_string_t explicit_dir = core_string_from_cstr(
        getenv("N00BB_NATIVE_CACHE_DIR"));
    core_string_t home = core_string_from_cstr(getenv("HOME"));
    core_string_t xdg = core_string_from_cstr(getenv("XDG_CACHE_HOME"));
    core_buffer_t directory = {0};
    if (explicit_dir.u8_bytes)
        core_buffer_append_string(&directory, explicit_dir);
    else if (xdg.u8_bytes) {
        core_buffer_append_string(&directory, xdg);
        core_buffer_puts(&directory, "/n00bb/native");
    }
    else if (home.u8_bytes) {
        core_buffer_append_string(&directory, home);
        core_buffer_puts(&directory, "/.cache/n00bb/native");
    }
    core_string_t root = directory.data
            && cache_mkdirs(core_buffer_string(&directory))
        ? core_string_dup(core_buffer_string(&directory)) : (core_string_t){};
    core_buffer_release(&directory);
    core_free(explicit_dir.data);
    core_free(home.data);
    core_free(xdg.data);
    return root;
#endif
}

n00bb_native_cache_t *
n00bb_native_cache_open(core_string_t executable_path)
{
#ifdef _WIN32
    (void)executable_path;
    return nullptr;
#else
    if (getenv("N00BB_NO_NATIVE_CACHE")) return nullptr;
    n00bb_native_cache_t *cache = core_alloc(n00bb_native_cache_t);
    if (cache) cache->directory = n00bb_native_cache_root();
    if (!cache || !cache->directory.data
        || !cache_compiler_hash(executable_path, &cache->compiler_hash)) {
        n00bb_native_cache_close(cache);
        return nullptr;
    }
    return cache;
#endif
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
#ifdef _WIN32
    (void)cache; (void)key; (void)object;
    return false;
#else
    if (!cache || !key || !object) return false;
    core_buffer_t path = cache_file_path(cache, key);
    FILE *file = path.data ? fopen(path.data, "rb") : nullptr;
    core_buffer_release(&path);
    if (!file) return false;
    bool ok = !fseek(file, 0, SEEK_END);
    long file_bytes = ok ? ftell(file) : -1;
    ok = file_bytes > (long)sizeof(cache_header_t)
        && !fseek(file, 0, SEEK_SET);
    cache_header_t header = {0};
    if (ok) ok = fread(&header, 1, sizeof header, file) == sizeof header;
    if (ok) ok = header.magic == CACHE_MAGIC
        && header.key_low == (uint64_t)key
        && header.key_high == (uint64_t)(key >> 64)
        && header.object_bytes
            == (uint64_t)(file_bytes - (long)sizeof header);
    unsigned char *bytes = ok ? malloc((size_t)header.object_bytes) : nullptr;
    if (ok) ok = bytes && fread(bytes, 1,
        (size_t)header.object_bytes, file) == (size_t)header.object_bytes;
    if (ok) {
        core_uint128_t hash = core_xxh3_128bits_raw(bytes,
            (size_t)header.object_bytes);
        ok = (uint64_t)hash == header.object_hash_low
            && (uint64_t)(hash >> 64) == header.object_hash_high;
    }
    if (ok) core_buffer_append(object, (const char *)bytes,
        (size_t)header.object_bytes);
    core_free(bytes);
    fclose(file);
    return ok;
#endif
}

void
n00bb_native_cache_write(const n00bb_native_cache_t *cache,
    core_uint128_t key, const void *object, size_t object_bytes)
{
#ifndef _WIN32
    if (!cache || !key || !object || !object_bytes) return;
    core_buffer_t path = cache_file_path(cache, key);
    if (!path.data) return;
    core_buffer_t temp = {0};
    core_buffer_append_string(&temp, core_buffer_string(&path));
    core_buffer_puts(&temp, ".XXXXXX");
    int fd = temp.data ? mkstemp(temp.data) : -1;
    FILE *file = fd >= 0 ? fdopen(fd, "wb") : nullptr;
    if (!file && fd >= 0) close(fd);
    core_uint128_t hash = core_xxh3_128bits_raw(object, object_bytes);
    cache_header_t header = {
        .magic = CACHE_MAGIC,
        .key_low = (uint64_t)key,
        .key_high = (uint64_t)(key >> 64),
        .object_bytes = object_bytes,
        .object_hash_low = (uint64_t)hash,
        .object_hash_high = (uint64_t)(hash >> 64),
    };
    bool ok = file
        && fwrite(&header, 1, sizeof header, file) == sizeof header
        && fwrite(object, 1, object_bytes, file) == object_bytes;
    if (file && fclose(file)) ok = false;
    if (ok && rename(temp.data, path.data)) ok = false;
    if (temp.data) unlink(temp.data);
    core_buffer_release(&temp);
    core_buffer_release(&path);
#else
    (void)cache; (void)key; (void)object; (void)object_bytes;
#endif
}
