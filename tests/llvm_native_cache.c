#include "llvm_native_cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
    (void)argc;
    (void)argv;
#else
    assert(argc > 0 && argv[0]);
    char directory[] = "/tmp/n00bb-native-cache-test-XXXXXX";
    assert(mkdtemp(directory));
    assert(!setenv("N00BB_NATIVE_CACHE_DIR", directory, 1));
    core_string_t directory_name = core_string_from_cstr(directory);
    core_string_t executable = core_string_from_cstr(argv[0]);
    n00bb_native_cache_t *cache = n00bb_native_cache_open(executable);
    assert(cache);
    const char bitcode[] = "bitcode fixture";
    core_string_t triple = CORE_STRING_STATIC("arm64-macos");
    core_string_t generic = CORE_STRING_STATIC("generic");
    core_string_t no_features = CORE_STRING_STATIC("");
    core_uint128_t key = n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, triple, generic, no_features, false, true);
    assert(key);
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, triple, CORE_STRING_STATIC("native"), no_features,
        false, true));
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, triple, generic, no_features, true, true));
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, triple, generic, no_features, false, false));
    const char object[] = "native object fixture";
    core_buffer_t readback = {0};
    assert(!n00bb_native_cache_read(cache, key, &readback));
    n00bb_native_cache_write(cache, key, object, sizeof object);
    assert(n00bb_native_cache_read(cache, key, &readback));
    assert(readback.byte_len == sizeof object
        && !memcmp(readback.data, object, sizeof object));
    core_buffer_release(&readback);

    char path[256];
    int length = snprintf(path, sizeof path, "%.*s/%016llx%016llx.o",
        (int)directory_name.u8_bytes, directory_name.data,
        (unsigned long long)(key >> 64),
        (unsigned long long)key);
    assert(length > 0 && (size_t)length < sizeof path);
    FILE *damaged = fopen(path, "r+b");
    assert(damaged && !fseek(damaged, -1, SEEK_END)
        && fputc('x', damaged) != EOF && !fclose(damaged));
    assert(!n00bb_native_cache_read(cache, key, &readback));
    n00bb_native_cache_write(cache, key, object, sizeof object);
    assert(n00bb_native_cache_read(cache, key, &readback));
    core_buffer_release(&readback);
    FILE *file = fopen(path, "wb");
    assert(file && fwrite("bad", 1, 3, file) == 3 && !fclose(file));
    assert(!n00bb_native_cache_read(cache, key, &readback));
    n00bb_native_cache_write(cache, key, object, sizeof object);
    assert(n00bb_native_cache_read(cache, key, &readback));
    core_buffer_release(&readback);
    n00bb_native_cache_close(cache);
    char blocked[256];
    length = snprintf(blocked, sizeof blocked, "%.*s/not-a-directory",
        (int)directory_name.u8_bytes, directory_name.data);
    assert(length > 0 && (size_t)length < sizeof blocked);
    file = fopen(blocked, "wb");
    assert(file && !fclose(file));
    assert(!setenv("N00BB_NATIVE_CACHE_DIR", blocked, 1));
    assert(!n00bb_native_cache_open(executable));
    assert(!unlink(blocked));
    char nested[256];
    length = snprintf(nested, sizeof nested, "%.*s/new-directory",
        (int)directory_name.u8_bytes, directory_name.data);
    assert(length > 0 && (size_t)length < sizeof nested);
    assert(!setenv("N00BB_NATIVE_CACHE_DIR", nested, 1));
    cache = n00bb_native_cache_open(executable);
    assert(cache);
    n00bb_native_cache_close(cache);
    assert(!rmdir(nested));
    assert(!unlink(path) && !rmdir(directory));
    core_free(executable.data);
    core_free(directory_name.data);
#endif
    return 0;
}
