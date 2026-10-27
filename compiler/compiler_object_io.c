#include "lib/os.h"
#include "compiler_object_io.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compiler_lower.h"
#include "driver.h"
#include "lib/alloc.h"

const char *const *
n00bb_system_libraries(const core_target_desc_t *target)
{
    static const char *const mach_o[] = {nullptr};
    static const char *const elf[] = {nullptr};
    static const char *const coff[] = {"-lsynchronization", nullptr};
    return target->object_format == CORE_TARGET_OBJECT_MACH_O ? mach_o
        : target->object_format == CORE_TARGET_OBJECT_COFF ? coff : elf;
}

core_string_t
n00bb_write_temporary_object(const void *bytes, size_t length)
{
    if (!bytes || !length) return (core_string_t){};
    core_string_t root = core_os_temp_root();
    core_buffer_t stem = {0};
    core_buffer_append_string(&stem, root);
    core_buffer_puts(&stem, "/n00bb-code-");
    core_free(root.data);
    core_string_t made = {};
    int fd = core_os_make_unique_file(core_buffer_string(&stem), &made);
    core_buffer_release(&stem);
    char *path = fd >= 0 ? core_string_to_cstr(made) : nullptr;
    core_free(made.data);
    if (fd < 0 || !path) {
        if (fd >= 0) close(fd);
        return (core_string_t){};
    }
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
        core_free(path);
        return (core_string_t){};
    }
    core_string_t result = core_string_from_cstr(path);
    if (!result.data) unlink(path);
    core_free(path);
    return result;
}

core_string_t
n00bb_archive_path(core_string_t archive_name)
{
    core_target_desc_t target;
    core_string_t runtime = n00bb_toolchain()->runtime;
    bool cross = n00bb_cross_target(&target);
    core_string_t directory = cross ? core_os_path_join(runtime, target.triple)
                                    : runtime;
    core_string_t path = core_os_path_join(directory, archive_name);
    if (cross) core_free(directory.data);
    if (core_os_is_regular_file(path)) return path;
    core_free(path.data);
    return (core_string_t){};
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
