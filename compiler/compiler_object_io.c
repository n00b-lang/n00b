#include "compiler_object_io.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver.h"
#include "lib/alloc.h"

#ifdef __APPLE__
const char *const n00bb_system_libraries[N00BB_SYSTEM_LIBRARY_COUNT + 1] = {nullptr};
#else
const char *const n00bb_system_libraries[N00BB_SYSTEM_LIBRARY_COUNT + 1] = {"-lm", nullptr};
#endif

core_string_t
n00bb_write_temporary_object(const void *bytes, size_t length)
{
    if (!bytes || !length) return (core_string_t){};
    char path[] = "/tmp/n00bb-code-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return (core_string_t){};
    bool ok = true;
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, (const char *)bytes + offset,
                                length - offset);
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += (size_t)written;
    }
    if (close(fd)) ok = false;
    if (!ok) {
        unlink(path);
        return (core_string_t){};
    }
    core_string_t result = core_string_from_raw(path, sizeof(path) - 1);
    if (!result.data) unlink(path);
    return result;
}

static bool
readable_path(const core_buffer_t *path)
{
    char *native_path = core_string_to_cstr(core_buffer_string(path));
    bool readable = native_path && !access(native_path, R_OK);
    core_free(native_path);
    return readable;
}

core_string_t
n00bb_archive_path(core_string_t compiler_path, core_string_t archive_name)
{
    core_string_t executable = n00bb_executable_path(compiler_path);
    int64_t slash = executable.data
        ? core_string_rfind_byte(executable, '/') : -1;
    core_buffer_t archive = {0};
    if (slash >= 0) {
        core_buffer_printf(&archive, "%.*s/%.*s",
            (int)slash, executable.data,
            (int)archive_name.u8_bytes, archive_name.data);
        if (!readable_path(&archive)) {
            core_buffer_release(&archive);
            core_buffer_printf(&archive, "%.*s/../lib/%.*s",
                (int)slash, executable.data,
                (int)archive_name.u8_bytes, archive_name.data);
        }
    }
    core_string_t path = readable_path(&archive)
        ? core_string_dup(core_buffer_string(&archive)) : (core_string_t){};
    core_buffer_release(&archive);
    core_free(executable.data);
    return path;
}

bool
n00bb_read_exact(int fd, void *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t size = read(fd, (uint8_t *)data + offset, length - offset);
        if (size <= 0) return false;
        offset += (size_t)size;
    }
    return true;
}

bool
n00bb_read_bitcode(core_string_t path, core_buffer_t *out)
{
    if (!path.data) return false;
    char *native_path = core_string_to_cstr(path);
    int fd = open(native_path, O_RDONLY);
    core_free(native_path);
    if (fd < 0) return false;
    struct stat info;
    bool ok = !fstat(fd, &info) && info.st_size > 0
        && (uintmax_t)info.st_size <= SIZE_MAX;
    void *bytes = ok ? malloc((size_t)info.st_size) : nullptr;
    ok = ok && bytes && n00bb_read_exact(fd, bytes, (size_t)info.st_size);
    if (ok) core_buffer_append(out, bytes, (size_t)info.st_size);
    free(bytes);
    close(fd);
    return ok;
}
