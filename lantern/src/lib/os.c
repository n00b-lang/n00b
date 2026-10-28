#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1

#include "lib/os.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "lib/alloc.h"
#include "lib/buffer.h"

enum { unique_suffix = 6 };

#ifdef _WIN32
#include "os_internal.h"
#include <io.h>

wchar_t *
core_os_wide(core_string_t text)
{
    if (text.u8_bytes > INT32_MAX) return nullptr;
    int units = text.u8_bytes
        ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data,
                              (int)text.u8_bytes, nullptr, 0)
        : 0;
    if (text.u8_bytes && units <= 0) return nullptr;
    wchar_t *wide = core_alloc_array(wchar_t, (size_t)units + 1);
    if (units)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data,
                            (int)text.u8_bytes, wide, units);
    return wide;
}

core_string_t
core_os_utf8(const wchar_t *wide, int units)
{
    int bytes = units ? WideCharToMultiByte(CP_UTF8, 0, wide, units, nullptr,
                                            0, nullptr, nullptr)
                      : 0;
    if (bytes <= 0) return core_string_dup(CORE_STRING_STATIC(""));
    char *text = core_alloc_size((size_t)bytes, 1);
    WideCharToMultiByte(CP_UTF8, 0, wide, units, text, bytes, nullptr,
                        nullptr);
    size_t length = (size_t)bytes - (units < 0 ? 1 : 0);
    core_string_t result = core_string_dup(core_string_borrow(text, length));
    core_free(text);
    return result;
}

core_string_t
core_os_utf8_path(const wchar_t *wide, int units)
{
    core_string_t path = core_os_utf8(wide, units);
    for (size_t i = 0; i < path.u8_bytes; i++)
        if (path.data[i] == '\\') path.data[i] = '/';
    return path;
}

int
core_os_errno(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
        return EACCES;
    case ERROR_SHARING_VIOLATION:
        return EBUSY;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    default:
        return EIO;
    }
}

core_string_t
core_os_real_path(core_string_t path)
{
    wchar_t *wide = path.data ? core_os_wide(path) : nullptr;
    HANDLE file = wide ? CreateFileW(wide, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)
        : INVALID_HANDLE_VALUE;
    core_free(wide);
    if (file == INVALID_HANDLE_VALUE) return (core_string_t){};
    wchar_t buffer[32768];
    DWORD capacity = sizeof(buffer) / sizeof(buffer[0]);
    DWORD units = GetFinalPathNameByHandleW(file, buffer, capacity,
                                            VOLUME_NAME_DOS);
    CloseHandle(file);
    if (!units || units >= capacity) return (core_string_t){};
    wchar_t *start = buffer;
    if (units >= 4 && buffer[0] == L'\\' && buffer[1] == L'\\'
        && buffer[2] == L'?' && buffer[3] == L'\\') {
        start += 4;
        units -= 4;
        if (units >= 4 && start[0] == L'U' && start[1] == L'N'
            && start[2] == L'C' && start[3] == L'\\') {
            start += 2;
            units -= 2;
            start[0] = L'\\';
        }
    }
    return core_os_utf8_path(start, (int)units);
}

bool
core_os_symlink_directory(core_string_t target, core_string_t link)
{
    wchar_t *destination = core_os_wide(target);
    wchar_t *name = core_os_wide(link);
    for (wchar_t *at = destination; at && *at; at++)
        if (*at == L'/') *at = L'\\';
    bool made = destination && name
        && CreateSymbolicLinkW(name, destination,
               SYMBOLIC_LINK_FLAG_DIRECTORY
                   | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    core_free(destination);
    core_free(name);
    return made;
}

core_string_t
core_os_current_directory(void)
{
    DWORD capacity = GetCurrentDirectoryW(0, nullptr);
    wchar_t *wide = capacity ? core_alloc_array(wchar_t, capacity) : nullptr;
    DWORD units = wide ? GetCurrentDirectoryW(capacity, wide) : 0;
    core_string_t path = units && units < capacity
        ? core_os_utf8_path(wide, (int)units) : (core_string_t){};
    core_free(wide);
    return path;
}

core_string_t
core_os_temp_root(void)
{
    wchar_t buffer[MAX_PATH + 2];
    DWORD capacity = sizeof(buffer) / sizeof(buffer[0]);
    DWORD units = GetTempPathW(capacity, buffer);
    if (!units || units >= capacity)
        return core_string_dup(CORE_STRING_STATIC("."));
    while (units && (buffer[units - 1] == L'\\' || buffer[units - 1] == L'/'))
        units--;
    return core_os_utf8_path(buffer, (int)units);
}

static void
fill_unique(char *suffix)
{
    static volatile LONG counter;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t seed = (uint64_t)now.QuadPart
        ^ ((uint64_t)GetCurrentProcessId() << 20)
        ^ (uint64_t)InterlockedIncrement(&counter) * 0x9e3779b97f4a7c15ull;
    static const char digits[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < unique_suffix; i++) {
        suffix[i] = digits[seed % 36];
        seed = seed / 36 ^ (seed << 7);
    }
}

static int
create_unique(core_string_t stem, bool directory, int flags,
              core_string_t *path)
{
    core_buffer_t name = {0};
    core_buffer_append_string(&name, stem);
    core_buffer_append(&name, "XXXXXX", unique_suffix);
    int fd = -1;
    bool made = false;
    for (int tries = 0; !made && tries < 100; tries++) {
        fill_unique(name.data + stem.u8_bytes);
        wchar_t *wide = core_os_wide(core_buffer_string(&name));
        if (!wide) break;
        if (directory) made = CreateDirectoryW(wide, nullptr);
        else {
            fd = _wopen(wide, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY
                        | _O_NOINHERIT | flags, _S_IREAD | _S_IWRITE);
            made = fd >= 0;
        }
        bool collided = !made && (directory
            ? GetLastError() == ERROR_ALREADY_EXISTS : errno == EEXIST);
        core_free(wide);
        if (!made && !collided) break;
    }
    if (made && path) *path = core_string_dup(core_buffer_string(&name));
    core_buffer_release(&name);
    return made ? (directory ? 0 : fd) : -1;
}

core_string_t
core_os_make_unique_directory(core_string_t stem)
{
    core_string_t path = {};
    return create_unique(stem, true, 0, &path) == 0 ? path
                                                    : (core_string_t){};
}

int
core_os_make_unique_file(core_string_t stem, core_string_t *path)
{
    return create_unique(stem, false, 0, path);
}

int
core_os_temp_fd(void)
{
    core_string_t root = core_os_temp_root();
    core_buffer_t stem = {0};
    core_buffer_append_string(&stem, root);
    core_buffer_append(&stem, "/n00bb-", 7);
    int fd = create_unique(core_buffer_string(&stem), false, _O_TEMPORARY,
                           nullptr);
    core_buffer_release(&stem);
    core_free(root.data);
    return fd;
}

int
core_os_cpu_count(void)
{
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count ? (int)count : 1;
}

int
core_os_terminal_columns(int fd)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE
        || !GetConsoleScreenBufferInfo(handle, &info)) return 0;
    return info.srWindow.Right - info.srWindow.Left + 1;
}

core_string_t
core_os_environment(core_string_t name)
{
    wchar_t *wide_name = core_os_wide(name);
    DWORD capacity = wide_name ? GetEnvironmentVariableW(wide_name, nullptr, 0)
                               : 0;
    wchar_t *wide = capacity ? core_alloc_array(wchar_t, capacity) : nullptr;
    DWORD units = wide ? GetEnvironmentVariableW(wide_name, wide, capacity) : 0;
    core_string_t value = units && units < capacity
        ? core_os_utf8(wide, (int)units) : (core_string_t){};
    core_free(wide);
    core_free(wide_name);
    return value;
}

bool
core_os_set_environment(core_string_t name, core_string_t value)
{
    wchar_t *wide_name = core_os_wide(name);
    wchar_t *wide_value = core_os_wide(value);
    bool set = wide_name && wide_value
        && _wputenv_s(wide_name, wide_value) == 0;
    core_free(wide_name);
    core_free(wide_value);
    return set;
}

bool
core_os_unset_environment(core_string_t name)
{
    wchar_t *wide_name = core_os_wide(name);
    bool unset = wide_name && _wputenv_s(wide_name, L"") == 0;
    core_free(wide_name);
    return unset;
}

core_string_t
core_os_executable_path(core_string_t argv0)
{
    DWORD capacity = 256;
    wchar_t *wide = nullptr;
    core_string_t path = {};
    for (;;) {
        wide = core_realloc(wide, capacity * sizeof(*wide));
        DWORD units = GetModuleFileNameW(nullptr, wide, capacity);
        if (!units) break;
        if (units < capacity - 1) {
            path = core_os_utf8_path(wide, (int)units);
            break;
        }
        capacity *= 2;
    }
    core_free(wide);
    if (!path.data) return argv0.data ? core_os_real_path(argv0) : path;
    core_string_t resolved = core_os_real_path(path);
    if (!resolved.data) return path;
    core_free(path.data);
    return resolved;
}

void
core_os_binary_io(void)
{
    _set_fmode(_O_BINARY);
    for (int fd = 0; fd < 3; fd++) _setmode(fd, _O_BINARY);
}

bool
core_os_make_directory(core_string_t path, unsigned mode)
{
    (void)mode;
    wchar_t *wide = core_os_wide(path);
    bool made = wide && (CreateDirectoryW(wide, nullptr)
                         || GetLastError() == ERROR_ALREADY_EXISTS);
    core_free(wide);
    return made;
}

bool
core_os_set_mode(int fd, unsigned mode)
{
    (void)fd;
    (void)mode;
    return true;
}

typedef struct {
    DWORD flags;
    HANDLE root;
    DWORD bytes;
    WCHAR name[];
} posix_rename_t;

enum {
    rename_info_ex = 22,
    rename_replace_if_exists = 0x1,
    rename_posix_semantics = 0x2,
};

static bool
posix_rename(const wchar_t *source, const wchar_t *target, DWORD *error)
{
    DWORD units = GetFullPathNameW(target, 0, nullptr, nullptr);
    size_t bytes = units ? sizeof(posix_rename_t)
        + (4 + (size_t)units) * sizeof(WCHAR) : 0;
    posix_rename_t *info = bytes ? core_alloc_size(bytes, 1) : nullptr;
    DWORD written = info ? GetFullPathNameW(target, units, info->name + 4,
                                            nullptr) : 0;
    HANDLE file = written && written < units
        ? CreateFileW(source, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE
                      | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS
                      | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)
        : INVALID_HANDLE_VALUE;
    bool renamed = false;
    if (file != INVALID_HANDLE_VALUE) {
        info->name[0] = L'\\';
        info->name[1] = L'\\';
        info->name[2] = L'?';
        info->name[3] = L'\\';
        info->flags = rename_replace_if_exists | rename_posix_semantics;
        info->bytes = (4 + written) * sizeof(WCHAR);
        renamed = SetFileInformationByHandle(file,
            (FILE_INFO_BY_HANDLE_CLASS)rename_info_ex, info, (DWORD)bytes);
        *error = renamed ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
    }
    else *error = info ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
    core_free(info);
    return renamed;
}

bool
core_os_rename_replace(core_string_t from, core_string_t to)
{
    wchar_t *source = core_os_wide(from);
    wchar_t *target = core_os_wide(to);
    DWORD attributes = target ? GetFileAttributesW(target)
                              : INVALID_FILE_ATTRIBUTES;
    if (attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && (attributes & FILE_ATTRIBUTE_DIRECTORY))
        RemoveDirectoryW(target);
    DWORD error = ERROR_INVALID_NAME;
    bool moved = source && target && posix_rename(source, target, &error);
    if (!moved && source && target) {
        moved = MoveFileExW(source, target, MOVEFILE_REPLACE_EXISTING);
        error = moved ? ERROR_SUCCESS : GetLastError();
    }
    if (!moved) SetLastError(error);
    if (!moved) errno = core_os_errno(GetLastError());
    core_free(source);
    core_free(target);
    return moved;
}

bool
core_os_is_executable(core_string_t path)
{
    wchar_t *wide = core_os_wide(path);
    DWORD attributes = wide ? GetFileAttributesW(wide)
                            : INVALID_FILE_ATTRIBUTES;
    core_free(wide);
    return attributes != INVALID_FILE_ATTRIBUTES
        && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

void *
core_os_map(int fd, size_t size, bool writable, bool private_copy,
            void *requested)
{
    HANDLE file = (HANDLE)_get_osfhandle(fd);
    if (file == INVALID_HANDLE_VALUE || !size) return nullptr;
    DWORD protection = private_copy ? PAGE_WRITECOPY
                     : writable ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE section = CreateFileMappingW(file, nullptr, protection,
        (DWORD)((uint64_t)size >> 32), (DWORD)size, nullptr);
    if (!section) return nullptr;
    DWORD access = private_copy ? FILE_MAP_COPY
                 : writable ? FILE_MAP_WRITE : FILE_MAP_READ;
    void *view = MapViewOfFileEx(section, access, 0, 0, size, requested);
    CloseHandle(section);
    return view;
}

long long
core_os_read_at(int fd, void *data, size_t bytes, size_t offset)
{
    OVERLAPPED position = {
        .Offset = (DWORD)offset, .OffsetHigh = (DWORD)((uint64_t)offset >> 32),
    };
    DWORD got = 0;
    return ReadFile((HANDLE)_get_osfhandle(fd), data, (DWORD)bytes, &got,
                    &position) ? (long long)got : -1;
}

bool
core_os_unmap(void *mapping, size_t size)
{
    (void)size;
    return UnmapViewOfFile(mapping) != 0;
}

bool
core_os_sync(void *mapping, size_t size, int fd)
{
    return FlushViewOfFile(mapping, size)
        && FlushFileBuffers((HANDLE)_get_osfhandle(fd));
}

static bool
missing_error(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        || error == ERROR_DIRECTORY;
}

static core_os_kind_t
attribute_kind(DWORD attributes)
{
    if (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
        return CORE_OS_KIND_OTHER;
    return attributes & FILE_ATTRIBUTE_DIRECTORY ? CORE_OS_KIND_DIRECTORY
                                                 : CORE_OS_KIND_FILE;
}

static uint64_t
filetime_value(FILETIME time)
{
    return ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

static HANDLE
open_handle(core_string_t path, DWORD access, DWORD flags, DWORD *error)
{
    wchar_t *wide = core_os_wide(path);
    HANDLE file = wide ? CreateFileW(wide, access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, flags, nullptr) : INVALID_HANDLE_VALUE;
    *error = !wide ? ERROR_INVALID_NAME
           : file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    core_free(wide);
    return file;
}

core_os_lookup_t
core_os_stat(core_string_t path, bool follow, core_os_file_info_t *info)
{
    DWORD error = ERROR_SUCCESS;
    HANDLE file = open_handle(path, 0, FILE_FLAG_BACKUP_SEMANTICS
        | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), &error);
    if (file == INVALID_HANDLE_VALUE)
        return missing_error(error) ? CORE_OS_MISSING : CORE_OS_FAILED;
    BY_HANDLE_FILE_INFORMATION identity;
    FILE_BASIC_INFO times;
    bool ok = GetFileInformationByHandle(file, &identity)
        && GetFileInformationByHandleEx(file, FileBasicInfo, &times,
                                        sizeof times);
    CloseHandle(file);
    if (!ok) return CORE_OS_FAILED;
    *info = (core_os_file_info_t){
        .kind = attribute_kind(identity.dwFileAttributes),
        .size = ((uint64_t)identity.nFileSizeHigh << 32)
            | identity.nFileSizeLow,
        .modified = filetime_value(identity.ftLastWriteTime),
        .changed = (uint64_t)times.ChangeTime.QuadPart,
        .device = identity.dwVolumeSerialNumber,
        .inode = ((uint64_t)identity.nFileIndexHigh << 32)
            | identity.nFileIndexLow,
        .mode = identity.dwFileAttributes & FILE_ATTRIBUTE_READONLY
            ? 0444 : 0644,
    };
    return CORE_OS_FOUND;
}

struct core_os_dir_t {
    HANDLE search;
    WIN32_FIND_DATAW item;
    bool pending;
    core_string_t name;
};

core_os_dir_t *
core_os_dir_open(core_string_t path, core_os_lookup_t *status)
{
    core_buffer_t pattern = {0};
    core_buffer_append_string(&pattern, path);
    if (path.u8_bytes && path.data[path.u8_bytes - 1] != '/'
        && path.data[path.u8_bytes - 1] != '\\')
        core_buffer_append(&pattern, "/", 1);
    core_buffer_append(&pattern, "*", 1);
    wchar_t *wide = core_os_wide(core_buffer_string(&pattern));
    core_buffer_release(&pattern);
    core_os_dir_t *dir = core_alloc(core_os_dir_t);
    dir->search = wide ? FindFirstFileW(wide, &dir->item)
                       : INVALID_HANDLE_VALUE;
    DWORD error = !wide ? ERROR_INVALID_NAME
                : dir->search == INVALID_HANDLE_VALUE ? GetLastError()
                : ERROR_SUCCESS;
    core_free(wide);
    if (dir->search == INVALID_HANDLE_VALUE) {
        core_free(dir);
        if (status)
            *status = missing_error(error) ? CORE_OS_MISSING : CORE_OS_FAILED;
        return nullptr;
    }
    dir->pending = true;
    if (status) *status = CORE_OS_FOUND;
    return dir;
}

bool
core_os_dir_next(core_os_dir_t *dir, core_string_t *name,
                 core_os_kind_t *kind)
{
    for (;;) {
        if (!dir->pending && !FindNextFileW(dir->search, &dir->item))
            return false;
        dir->pending = false;
        const wchar_t *raw = dir->item.cFileName;
        if (raw[0] == L'.' && (!raw[1] || (raw[1] == L'.' && !raw[2])))
            continue;
        core_free(dir->name.data);
        dir->name = core_os_utf8_path(raw, -1);
        if (!dir->name.data) continue;
        *name = dir->name;
        if (kind) *kind = attribute_kind(dir->item.dwFileAttributes);
        return true;
    }
}

void
core_os_dir_close(core_os_dir_t *dir)
{
    if (!dir) return;
    FindClose(dir->search);
    core_free(dir->name.data);
    core_free(dir);
}

bool
core_os_read_file(core_string_t path, char **bytes, size_t *size)
{
    DWORD error = ERROR_SUCCESS;
    HANDLE file = open_handle(path, GENERIC_READ, FILE_ATTRIBUTE_NORMAL,
                              &error);
    if (file == INVALID_HANDLE_VALUE) {
        errno = core_os_errno(error);
        return false;
    }
    LARGE_INTEGER length;
    bool ok = GetFileSizeEx(file, &length) && length.QuadPart >= 0
        && (uint64_t)length.QuadPart < SIZE_MAX;
    size_t total = ok ? (size_t)length.QuadPart : 0;
    char *data = ok ? core_alloc_size(1, total + 1) : nullptr;
    size_t done = 0;
    while (ok && done < total) {
        DWORD chunk = total - done > 0x40000000u ? 0x40000000u
                                                 : (DWORD)(total - done);
        DWORD got = 0;
        ok = ReadFile(file, data + done, chunk, &got, nullptr) && got;
        done += got;
    }
    CloseHandle(file);
    if (!ok) {
        core_free(data);
        return false;
    }
    *bytes = data;
    *size = total;
    return true;
}

bool
core_os_write_all(int fd, const void *data, size_t size)
{
    const char *at = data;
    while (size) {
        unsigned chunk = size > 0x40000000u ? 0x40000000u : (unsigned)size;
        int wrote = _write(fd, at, chunk);
        if (wrote <= 0) return false;
        at += wrote;
        size -= (size_t)wrote;
    }
    return true;
}

bool
core_os_flush(int fd)
{
    return _commit(fd) == 0;
}

bool
core_os_close(int fd)
{
    return _close(fd) == 0;
}

static bool
removed(wchar_t *wide, bool directory)
{
    if (!wide) {
        errno = EINVAL;
        return false;
    }
    bool done = directory ? RemoveDirectoryW(wide) : DeleteFileW(wide);
    if (!done && GetLastError() == ERROR_ACCESS_DENIED) {
        DWORD attributes = GetFileAttributesW(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_READONLY)
            && SetFileAttributesW(wide, attributes & ~FILE_ATTRIBUTE_READONLY))
            done = directory ? RemoveDirectoryW(wide) : DeleteFileW(wide);
    }
    if (!done) errno = core_os_errno(GetLastError());
    core_free(wide);
    return done;
}

bool
core_os_remove_file(core_string_t path)
{
    return removed(core_os_wide(path), false);
}

bool
core_os_remove_directory(core_string_t path)
{
    return removed(core_os_wide(path), true);
}

static bool
has_drive(core_string_t path)
{
    char letter = path.u8_bytes >= 2 ? path.data[0] : 0;
    return ((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))
        && path.data[1] == ':';
}

bool
core_os_is_absolute(core_string_t path)
{
    if (path.u8_bytes >= 3 && has_drive(path)
        && core_os_is_separator(path.data[2]))
        return true;
    return path.u8_bytes >= 2 && core_os_is_separator(path.data[0])
        && core_os_is_separator(path.data[1]);
}

core_string_t
core_os_root_relative(core_string_t absolute)
{
    if (!core_os_is_absolute(absolute)) return (core_string_t){};
    core_buffer_t relative = {0};
    if (has_drive(absolute)) {
        core_buffer_append(&relative, absolute.data, 1);
        core_buffer_append_string(&relative,
            core_string_slice(absolute, 2, absolute.u8_bytes));
    }
    else {
        core_buffer_append(&relative, "UNC/", 4);
        core_buffer_append_string(&relative,
            core_string_slice(absolute, 2, absolute.u8_bytes));
    }
    for (size_t i = 0; i < relative.byte_len; i++)
        if (relative.data[i] == '\\') relative.data[i] = '/';
    while (relative.byte_len && relative.data[relative.byte_len - 1] == '/')
        relative.byte_len--;
    core_string_t result = core_string_dup(core_buffer_string(&relative));
    core_buffer_release(&relative);
    return result;
}

#else
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

core_string_t
core_os_real_path(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    char *resolved = native ? realpath(native, nullptr) : nullptr;
    core_free(native);
    core_string_t result = core_string_from_cstr(resolved);
    free(resolved);
    return result;
}

core_string_t
core_os_temp_root(void)
{
    return core_string_dup(CORE_STRING_STATIC("/tmp"));
}

bool
core_os_symlink_directory(core_string_t target, core_string_t link)
{
    char *destination = core_string_to_cstr(target);
    char *name = core_string_to_cstr(link);
    bool made = destination && name && !symlink(destination, name);
    core_free(destination);
    core_free(name);
    return made;
}

core_string_t
core_os_current_directory(void)
{
    char *native = getcwd(nullptr, 0);
    core_string_t path = core_string_from_cstr(native);
    free(native);
    return path;
}

static char *
unique_template(core_string_t stem)
{
    core_buffer_t name = {0};
    core_buffer_append_string(&name, stem);
    core_buffer_append(&name, "XXXXXX", unique_suffix);
    char *native = core_string_to_cstr(core_buffer_string(&name));
    core_buffer_release(&name);
    return native;
}

core_string_t
core_os_make_unique_directory(core_string_t stem)
{
    char *native = unique_template(stem);
    core_string_t path = native && mkdtemp(native)
        ? core_string_dup(core_string_borrow(native,
              stem.u8_bytes + unique_suffix))
        : (core_string_t){};
    core_free(native);
    return path;
}

int
core_os_make_unique_file(core_string_t stem, core_string_t *path)
{
    char *native = unique_template(stem);
    int fd = native ? mkstemp(native) : -1;
    if (fd >= 0 && path)
        *path = core_string_dup(core_string_borrow(native,
            stem.u8_bytes + unique_suffix));
    core_free(native);
    return fd;
}

int
core_os_temp_fd(void)
{
    core_string_t path = {};
    int fd = core_os_make_unique_file(CORE_STRING_STATIC("/tmp/n00bb-"),
                                      &path);
    char *native = fd >= 0 ? core_string_to_cstr(path) : nullptr;
    if (native) unlink(native);
    core_free(native);
    core_free(path.data);
    return fd;
}

int
core_os_cpu_count(void)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (int)count : 1;
}

int
core_os_terminal_columns(int fd)
{
    struct winsize size;
    return ioctl(fd, TIOCGWINSZ, &size) == 0 ? size.ws_col : 0;
}

core_string_t
core_os_environment(core_string_t name)
{
    char *native = core_string_to_cstr(name);
    core_string_t value = native ? core_string_from_cstr(getenv(native))
                                 : (core_string_t){};
    core_free(native);
    return value;
}

bool
core_os_set_environment(core_string_t name, core_string_t value)
{
    char *native_name = core_string_to_cstr(name);
    char *native_value = core_string_to_cstr(value);
    bool set = native_name && native_value
        && setenv(native_name, native_value, 1) == 0;
    core_free(native_name);
    core_free(native_value);
    return set;
}

bool
core_os_unset_environment(core_string_t name)
{
    char *native = core_string_to_cstr(name);
    bool unset = native && unsetenv(native) == 0;
    core_free(native);
    return unset;
}

core_string_t
core_os_executable_path(core_string_t argv0)
{
#if defined(__APPLE__)
    uint32_t capacity = 0;
    _NSGetExecutablePath(nullptr, &capacity);
    char *path = core_alloc_size(1, capacity + 1);
    core_string_t found = _NSGetExecutablePath(path, &capacity) == 0
        ? core_string_from_cstr(path) : (core_string_t){};
    core_free(path);
#elif defined(__linux__)
    core_string_t found = core_os_real_path(CORE_STRING_STATIC("/proc/self/exe"));
#else
    core_string_t found = {};
#endif
    if (!found.data) return argv0.data ? core_os_real_path(argv0) : found;
    core_string_t resolved = core_os_real_path(found);
    if (!resolved.data) return found;
    core_free(found.data);
    return resolved;
}

void
core_os_binary_io(void)
{
}

bool
core_os_make_directory(core_string_t path, unsigned mode)
{
    char *native = core_string_to_cstr(path);
    bool made = native && (mkdir(native, (mode_t)mode) == 0 || errno == EEXIST);
    core_free(native);
    return made;
}

bool
core_os_set_mode(int fd, unsigned mode)
{
    return fchmod(fd, (mode_t)mode) == 0;
}

bool
core_os_rename_replace(core_string_t from, core_string_t to)
{
    char *source = core_string_to_cstr(from);
    char *target = core_string_to_cstr(to);
    bool moved = source && target && rename(source, target) == 0;
    core_free(source);
    core_free(target);
    return moved;
}

bool
core_os_is_executable(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    bool executable = native && access(native, X_OK) == 0;
    core_free(native);
    return executable;
}

void *
core_os_map(int fd, size_t size, bool writable, bool private_copy,
            void *requested)
{
    void *mapping = mmap(requested, size,
        PROT_READ | (writable ? PROT_WRITE : 0),
        private_copy ? MAP_PRIVATE : MAP_SHARED, fd, 0);
    return mapping == MAP_FAILED ? nullptr : mapping;
}

long long
core_os_read_at(int fd, void *data, size_t bytes, size_t offset)
{
    return pread(fd, data, bytes, (off_t)offset);
}

bool
core_os_unmap(void *mapping, size_t size)
{
    return munmap(mapping, size) == 0;
}

bool
core_os_sync(void *mapping, size_t size, int fd)
{
    return msync(mapping, size, MS_SYNC) == 0 && fsync(fd) == 0;
}

static uint64_t
timespec_value(struct timespec time)
{
    return (uint64_t)time.tv_sec * UINT64_C(1000000000)
        + (uint64_t)time.tv_nsec;
}

static core_os_kind_t
mode_kind(mode_t mode)
{
    return S_ISREG(mode) ? CORE_OS_KIND_FILE
         : S_ISDIR(mode) ? CORE_OS_KIND_DIRECTORY : CORE_OS_KIND_OTHER;
}

core_os_lookup_t
core_os_stat(core_string_t path, bool follow, core_os_file_info_t *info)
{
    char *native = core_string_to_cstr(path);
    if (!native) return CORE_OS_FAILED;
    struct stat status;
    int result = follow ? stat(native, &status) : lstat(native, &status);
    int error = errno;
    core_free(native);
    if (result != 0)
        return error == ENOENT || error == ENOTDIR ? CORE_OS_MISSING
                                                   : CORE_OS_FAILED;
#ifdef __APPLE__
    struct timespec modified = status.st_mtimespec;
    struct timespec changed = status.st_ctimespec;
#else
    struct timespec modified = status.st_mtim;
    struct timespec changed = status.st_ctim;
#endif
    *info = (core_os_file_info_t){
        .kind = mode_kind(status.st_mode),
        .size = (uint64_t)status.st_size,
        .modified = timespec_value(modified),
        .changed = timespec_value(changed),
        .device = (uint64_t)status.st_dev,
        .inode = (uint64_t)status.st_ino,
        .mode = (unsigned)status.st_mode & 07777,
    };
    return CORE_OS_FOUND;
}

struct core_os_dir_t {
    DIR *stream;
    core_string_t path;
    core_string_t name;
};

core_os_dir_t *
core_os_dir_open(core_string_t path, core_os_lookup_t *status)
{
    char *native = core_string_to_cstr(path);
    DIR *stream = native ? opendir(native) : nullptr;
    int error = native ? errno : EINVAL;
    core_free(native);
    if (!stream) {
        if (status)
            *status = error == ENOENT || error == ENOTDIR ? CORE_OS_MISSING
                                                          : CORE_OS_FAILED;
        return nullptr;
    }
    core_os_dir_t *dir = core_alloc(core_os_dir_t);
    dir->stream = stream;
    dir->path = core_string_dup(path);
    if (status) *status = CORE_OS_FOUND;
    return dir;
}

static core_os_kind_t
entry_kind(core_os_dir_t *dir, const struct dirent *item)
{
#ifdef DT_UNKNOWN
    if (item->d_type != DT_UNKNOWN)
        return item->d_type == DT_REG ? CORE_OS_KIND_FILE
             : item->d_type == DT_DIR ? CORE_OS_KIND_DIRECTORY
             : CORE_OS_KIND_OTHER;
#else
    (void)item;
#endif
    core_buffer_t path = {0};
    core_buffer_append_string(&path, dir->path);
    core_buffer_append(&path, "/", 1);
    core_buffer_append_string(&path, dir->name);
    core_os_file_info_t info = {0};
    bool found = core_os_stat(core_buffer_string(&path), false, &info)
        == CORE_OS_FOUND;
    core_buffer_release(&path);
    return found ? info.kind : CORE_OS_KIND_OTHER;
}

bool
core_os_dir_next(core_os_dir_t *dir, core_string_t *name,
                 core_os_kind_t *kind)
{
    struct dirent *item;
    while ((item = readdir(dir->stream))) {
        core_free(dir->name.data);
        dir->name = core_string_from_cstr(item->d_name);
        if (core_string_eq(dir->name, CORE_STRING_STATIC("."))
            || core_string_eq(dir->name, CORE_STRING_STATIC("..")))
            continue;
        *name = dir->name;
        if (kind) *kind = entry_kind(dir, item);
        return true;
    }
    return false;
}

void
core_os_dir_close(core_os_dir_t *dir)
{
    if (!dir) return;
    closedir(dir->stream);
    core_free(dir->path.data);
    core_free(dir->name.data);
    core_free(dir);
}

bool
core_os_read_file(core_string_t path, char **bytes, size_t *size)
{
    char *native = core_string_to_cstr(path);
    int fd = native ? open(native, O_RDONLY | O_CLOEXEC) : -1;
    core_free(native);
    if (fd < 0) return false;
    struct stat status;
    bool ok = fstat(fd, &status) == 0 && status.st_size >= 0
        && (uint64_t)status.st_size < SIZE_MAX;
    size_t total = ok ? (size_t)status.st_size : 0;
    char *data = ok ? core_alloc_size(1, total + 1) : nullptr;
    size_t done = 0;
    while (ok && done < total) {
        ssize_t got = read(fd, data + done, total - done);
        if (got < 0 && errno == EINTR) continue;
        ok = got > 0;
        if (ok) done += (size_t)got;
    }
    close(fd);
    if (!ok) {
        core_free(data);
        return false;
    }
    *bytes = data;
    *size = total;
    return true;
}

bool
core_os_write_all(int fd, const void *data, size_t size)
{
    const char *at = data;
    while (size) {
        ssize_t wrote = write(fd, at, size);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return false;
        at += wrote;
        size -= (size_t)wrote;
    }
    return true;
}

bool
core_os_flush(int fd)
{
    return fsync(fd) == 0;
}

bool
core_os_close(int fd)
{
    return close(fd) == 0;
}

bool
core_os_remove_file(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    bool removed = native && unlink(native) == 0;
    core_free(native);
    return removed;
}

bool
core_os_remove_directory(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    bool removed = native && rmdir(native) == 0;
    core_free(native);
    return removed;
}

bool
core_os_is_absolute(core_string_t path)
{
    return path.u8_bytes && path.data[0] == '/';
}

core_string_t
core_os_root_relative(core_string_t absolute)
{
    if (!core_os_is_absolute(absolute)) return (core_string_t){};
    return core_string_dup(core_string_slice(absolute, 1,
                                             absolute.u8_bytes - 1));
}
#endif

bool
core_os_is_separator(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static core_os_kind_t
kind_of(core_string_t path)
{
    core_os_file_info_t info = {};
    return core_os_stat(path, true, &info) == CORE_OS_FOUND
        ? info.kind : CORE_OS_KIND_OTHER;
}

bool
core_os_is_regular_file(core_string_t path)
{
    return kind_of(path) == CORE_OS_KIND_FILE;
}

bool
core_os_is_directory(core_string_t path)
{
    return kind_of(path) == CORE_OS_KIND_DIRECTORY;
}

core_string_t
core_os_path_join(core_string_t parent, core_string_t child)
{
    core_buffer_t path = {0};
    core_buffer_append_string(&path, parent);
    if (path.byte_len && !core_os_is_separator(path.data[path.byte_len - 1]))
        core_buffer_putc(&path, '/');
    core_buffer_append_string(&path, child);
    core_string_t joined = core_string_dup(core_buffer_string(&path));
    core_buffer_release(&path);
    return joined;
}

core_string_t
core_os_path_parent(core_string_t path)
{
    size_t length = path.u8_bytes;
    while (length > 1 && core_os_is_separator(path.data[length - 1]))
        length--;
    size_t root = 0;
#ifdef _WIN32
    if (length >= 2 && path.data[1] == ':') root = 2;
#endif
    size_t at = length;
    while (at > root && !core_os_is_separator(path.data[at - 1])) at--;
    if (at == root && !root) return core_string_dup(CORE_STRING_STATIC("."));
    size_t keep = at > root + 1 ? at - 1 : at;
    if (keep == root) {
        core_buffer_t drive = {0};
        core_buffer_append(&drive, path.data, root);
        core_buffer_putc(&drive, '/');
        core_string_t parent = core_string_dup(core_buffer_string(&drive));
        core_buffer_release(&drive);
        return parent;
    }
    return core_string_dup(core_string_slice(path, 0, keep));
}

core_string_t
core_os_find_upward(core_string_t name)
{
    core_string_t directory = core_os_current_directory();
    while (directory.data) {
        core_string_t candidate = core_os_path_join(directory, name);
        if (core_os_is_regular_file(candidate)) {
            core_free(directory.data);
            return candidate;
        }
        core_free(candidate.data);
        core_string_t parent = core_os_path_parent(directory);
        bool top = core_string_eq(parent, directory);
        core_free(directory.data);
        if (top) core_free(parent.data);
        directory = top ? (core_string_t){} : parent;
    }
    return (core_string_t){};
}

bool
core_os_write_file_atomic(core_string_t path, const void *data, size_t size)
{
    core_buffer_t stem = {0};
    core_buffer_append_string(&stem, path);
    core_buffer_puts(&stem, ".tmp.");
    core_string_t temporary = {};
    int fd = core_os_make_unique_file(core_buffer_string(&stem), &temporary);
    core_buffer_release(&stem);
    bool ok = fd >= 0 && core_os_write_all(fd, data, size);
    if (fd >= 0 && !core_os_close(fd)) ok = false;
    if (ok) ok = core_os_rename_replace(temporary, path);
    if (!ok && fd >= 0) core_os_remove_file(temporary);
    core_free(temporary.data);
    return ok;
}

static void
note_removal(core_os_removal_t *result, core_string_t path)
{
    if (!result || result->error) return;
    result->error = errno ? errno : EIO;
    result->failed_path = core_string_dup(path);
}

static void
remove_entry(core_string_t path, core_os_kind_t kind,
             core_os_removal_t *result)
{
    if (kind == CORE_OS_KIND_DIRECTORY) {
        core_os_lookup_t status = CORE_OS_FOUND;
        core_os_dir_t *dir = core_os_dir_open(path, &status);
        core_string_t name = {};
        core_os_kind_t child_kind = CORE_OS_KIND_OTHER;
        while (dir && core_os_dir_next(dir, &name, &child_kind)) {
            core_string_t child = core_os_path_join(path, name);
            remove_entry(child, child_kind, result);
            core_free(child.data);
        }
        core_os_dir_close(dir);
        if (!dir && status == CORE_OS_FAILED) note_removal(result, path);
        if (!core_os_remove_directory(path)) note_removal(result, path);
        return;
    }
    if (!core_os_remove_file(path) && !core_os_remove_directory(path))
        note_removal(result, path);
}

bool
core_os_remove_tree(core_string_t path, core_os_removal_t *result)
{
    core_os_removal_t ignored = {};
    if (!result) result = &ignored;
    *result = (core_os_removal_t){};
    core_os_file_info_t info = {};
    core_os_lookup_t found = core_os_stat(path, false, &info);
    if (found == CORE_OS_FAILED) {
        result->existed = true;
        note_removal(result, path);
    }
    if (found == CORE_OS_FOUND) {
        result->existed = true;
        remove_entry(path, info.kind, result);
    }
    bool ok = !result->error;
    if (result == &ignored) core_os_removal_release(&ignored);
    return ok;
}

void
core_os_removal_release(core_os_removal_t *result)
{
    core_free(result->failed_path.data);
    result->failed_path = (core_string_t){};
}
