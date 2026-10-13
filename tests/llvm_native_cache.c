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
    n00bb_native_cache_t *cache = n00bb_native_cache_open(argv[0]);
    assert(cache);
    const char bitcode[] = "bitcode fixture";
    core_uint128_t key = n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, "arm64-macos", "generic", "", false, true);
    assert(key);
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, "arm64-macos", "native", "", false, true));
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, "arm64-macos", "generic", "", true, true));
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, "arm64-macos", "generic", "", false, false));
    const char object[] = "native object fixture";
    core_buffer_t readback = {0};
    assert(!n00bb_native_cache_read(cache, key, &readback));
    n00bb_native_cache_write(cache, key, object, sizeof object);
    assert(n00bb_native_cache_read(cache, key, &readback));
    assert(readback.byte_len == sizeof object
        && !memcmp(readback.data, object, sizeof object));
    core_buffer_release(&readback);

    char path[256];
    int length = snprintf(path, sizeof path, "%s/%016llx%016llx.o",
        directory, (unsigned long long)(key >> 64),
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
    length = snprintf(blocked, sizeof blocked, "%s/not-a-directory",
        directory);
    assert(length > 0 && (size_t)length < sizeof blocked);
    file = fopen(blocked, "wb");
    assert(file && !fclose(file));
    assert(!setenv("N00BB_NATIVE_CACHE_DIR", blocked, 1));
    assert(!n00bb_native_cache_open(argv[0]));
    assert(!unlink(blocked));
    char nested[256];
    length = snprintf(nested, sizeof nested, "%s/new-directory", directory);
    assert(length > 0 && (size_t)length < sizeof nested);
    assert(!setenv("N00BB_NATIVE_CACHE_DIR", nested, 1));
    cache = n00bb_native_cache_open(argv[0]);
    assert(cache);
    n00bb_native_cache_close(cache);
    assert(!rmdir(nested));
    assert(!unlink(path) && !rmdir(directory));
#endif
    return 0;
}
