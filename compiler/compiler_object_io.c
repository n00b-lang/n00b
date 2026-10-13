#include "compiler_object_io.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver.h"
#include "lib/alloc.h"

char *
n00bb_write_temporary_object(const void *bytes, size_t length)
{
    if (!bytes || !length) return nullptr;
    char path[] = "/tmp/n00bb-code-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return nullptr;
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
        return nullptr;
    }
    char *result = core_strdup(path);
    if (!result) unlink(path);
    return result;
}

char *
n00bb_archive_path(const char *compiler_path, const char *archive_name)
{
    char *executable = n00bb_executable_path(compiler_path);
    const char *slash = executable ? strrchr(executable, '/') : nullptr;
    core_buffer_t archive = {0};
    if (slash) {
        core_buffer_printf(&archive, "%.*s/%s",
            (int)(slash - executable), executable, archive_name);
        if (access(archive.data, R_OK)) {
            core_buffer_release(&archive);
            core_buffer_printf(&archive, "%.*s/../lib/%s",
                (int)(slash - executable), executable, archive_name);
        }
    }
    char *path = archive.data && !access(archive.data, R_OK)
        ? core_strdup(archive.data) : nullptr;
    core_buffer_release(&archive);
    core_free(executable);
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
n00bb_read_bitcode(const char *path, core_buffer_t *out)
{
    int fd = open(path, O_RDONLY);
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
