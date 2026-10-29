#include "cache_location.h"
#include "llvm_native_cache.h"
#include "lib/alloc.h"

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
    core_string_t directory_name = core_string_from_cstr(directory);
    n00bb_cache_set_root(directory_name);
    core_string_t executable = core_string_from_cstr(argv[0]);
    n00bb_native_cache_t *cache = n00bb_native_cache_open(executable);
    assert(cache);
    const char bitcode[] = "bitcode fixture";
    core_string_t triple = CORE_STRING_STATIC("arm64-macos");
    core_string_t generic = CORE_STRING_STATIC("generic");
    core_string_t no_features = CORE_STRING_STATIC("");
    core_uint128_t key = n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, triple, generic, no_features, false, 3, true);
    assert(key);
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, triple, CORE_STRING_STATIC("native"), no_features,
        false, 3, true));
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, triple, generic, no_features, true, 3, true));
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, triple, generic, no_features, false, 0, true));
    assert(key != n00bb_native_cache_key(cache, bitcode,
        sizeof bitcode, triple, generic, no_features, false, 3, false));
    const char object[] = "native object fixture";
    core_buffer_t readback = {0};
    assert(!n00bb_native_cache_read(cache, key, &readback));
    n00bb_native_cache_write(cache, key, object, sizeof object);
    assert(n00bb_native_cache_read(cache, key, &readback));
    assert(readback.byte_len == sizeof object
        && !memcmp(readback.data, object, sizeof object));
    core_buffer_release(&readback);

    core_buffer_t text = {0};
    core_buffer_printf(&text, "%.*s/dev/native/%016llx%016llx.o",
        (int)directory_name.u8_bytes, directory_name.data,
        (unsigned long long)(key >> 64),
        (unsigned long long)key);
    char *path = core_string_to_cstr(core_buffer_string(&text));
    core_buffer_release(&text);
    assert(path);
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
    core_buffer_printf(&text, "%.*s/not-a-directory",
        (int)directory_name.u8_bytes, directory_name.data);
    char *blocked = core_string_to_cstr(core_buffer_string(&text));
    core_buffer_release(&text);
    assert(blocked);
    file = fopen(blocked, "wb");
    assert(file && !fclose(file));
    core_string_t blocked_root = core_string_from_cstr(blocked);
    n00bb_cache_set_root(blocked_root);
    core_free(blocked_root.data);
    assert(!n00bb_native_cache_open(executable));
    assert(!unlink(blocked));
    core_buffer_printf(&text, "%.*s/new-directory",
        (int)directory_name.u8_bytes, directory_name.data);
    char *nested = core_string_to_cstr(core_buffer_string(&text));
    core_buffer_release(&text);
    assert(nested);
    core_string_t nested_root = core_string_from_cstr(nested);
    n00bb_cache_set_root(nested_root);
    core_free(nested_root.data);
    cache = n00bb_native_cache_open(executable);
    assert(cache);
    n00bb_native_cache_close(cache);
    core_buffer_printf(&text, "%.*s/new-directory/dev/native",
        (int)directory_name.u8_bytes, directory_name.data);
    char *nested_native = core_string_to_cstr(core_buffer_string(&text));
    core_buffer_release(&text);
    core_buffer_printf(&text, "%.*s/new-directory/dev",
        (int)directory_name.u8_bytes, directory_name.data);
    char *nested_key = core_string_to_cstr(core_buffer_string(&text));
    core_buffer_release(&text);
    core_buffer_printf(&text, "%.*s/dev/native",
        (int)directory_name.u8_bytes, directory_name.data);
    char *native = core_string_to_cstr(core_buffer_string(&text));
    core_buffer_release(&text);
    core_buffer_printf(&text, "%.*s/dev",
        (int)directory_name.u8_bytes, directory_name.data);
    char *key_directory = core_string_to_cstr(core_buffer_string(&text));
    core_buffer_release(&text);
    assert(nested_native && nested_key && native && key_directory);
    assert(!rmdir(nested_native) && !rmdir(nested_key) && !rmdir(nested));
    assert(!unlink(path) && !rmdir(native) && !rmdir(key_directory)
           && !rmdir(directory));
    core_free(key_directory);
    core_free(nested_key);
    core_free(native);
    core_free(nested_native);
    core_free(nested);
    core_free(blocked);
    core_free(path);
    core_free(executable.data);
    core_free(directory_name.data);
#endif
    return 0;
}
