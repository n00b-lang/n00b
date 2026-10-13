#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1

#include "lib/snapshot_inventory.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <wchar.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/byteorder.h"
#include "lib/hash.h"
#include "lib/list.h"

typedef struct {
    uint64_t size;
    uint64_t modified;
    uint64_t changed;
    uint64_t device;
    uint64_t inode;
} file_signature_t;

struct core_source_snapshot_t {
    core_source_file_t source;
    uint64_t revision;
    size_t references;
};

typedef struct {
    size_t root_index;
    core_string_t relative;
    file_signature_t signature;
    core_source_snapshot_t *snapshot;
} inventory_entry_t;
core_list_decl(inventory_entry_t);

struct core_snapshot_inventory_t {
    core_list_t(core_string_t) roots;
    core_list_t(inventory_entry_t) entries;
    core_snapshot_inventory_accept_fn accept;
    void *context;
    uint64_t generation;
    uint64_t next_revision;
    core_string_t metadata_directory;
    uint64_t filter_revision;
    core_snapshot_inventory_stats_t stats;
};

static char *
join_path(const char *parent, const char *child)
{
    size_t a = strlen(parent), b = strlen(child);
    bool separator = a && parent[a - 1] != '/' && parent[a - 1] != '\\';
    char *path = core_alloc_size(1, a + b + (separator ? 2 : 1));
    memcpy(path, parent, a);
    if (separator) path[a++] = '/';
    memcpy(path + a, child, b);
    return path;
}

#ifdef _WIN32
static wchar_t *
wide_path(const char *path)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                                    nullptr, 0);
    if (length <= 0) return nullptr;
    wchar_t *wide = core_alloc_array(wchar_t, length);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                             wide, length)) {
        core_free(wide);
        return nullptr;
    }
    return wide;
}

static char *
utf8_name(const wchar_t *wide)
{
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                    nullptr, nullptr);
    if (length <= 0) return nullptr;
    char *name = core_alloc_array(char, length);
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, length,
                             nullptr, nullptr)) {
        core_free(name);
        return nullptr;
    }
    return name;
}

static FILE *
open_binary(const char *path)
{
    wchar_t *wide = wide_path(path);
    FILE *file = wide ? _wfopen(wide, L"rb") : nullptr;
    core_free(wide);
    return file;
}

static uint64_t
filetime_value(FILETIME time)
{
    return ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

static int
path_signature(const char *path, file_signature_t *signature,
               bool *directory, bool *regular)
{
    wchar_t *wide = wide_path(path);
    WIN32_FILE_ATTRIBUTE_DATA info;
    bool ok = wide && GetFileAttributesExW(wide, GetFileExInfoStandard,
                                           &info);
    DWORD error = wide && !ok ? GetLastError() : ERROR_INVALID_NAME;
    core_free(wide);
    if (!ok) {
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? 0 : -1;
    }
    bool reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    *directory = !reparse
        && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    *regular = !reparse && !*directory;
    *signature = (file_signature_t){
        .size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow,
        .modified = filetime_value(info.ftLastWriteTime),
        .changed = filetime_value(info.ftCreationTime),
    };
    return 1;
}
#else
static FILE *
open_binary(const char *path)
{
    return fopen(path, "rb");
}

static uint64_t
timespec_value(time_t seconds, long nanoseconds)
{
    return (uint64_t)seconds * UINT64_C(1000000000)
        + (uint64_t)nanoseconds;
}

static int
path_signature(const char *path, file_signature_t *signature,
               bool *directory, bool *regular)
{
    struct stat info;
    if (lstat(path, &info) != 0)
        return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    *directory = S_ISDIR(info.st_mode);
    *regular = S_ISREG(info.st_mode);
#ifdef __APPLE__
    uint64_t modified = timespec_value(info.st_mtimespec.tv_sec,
                                       info.st_mtimespec.tv_nsec);
    uint64_t changed = timespec_value(info.st_ctimespec.tv_sec,
                                      info.st_ctimespec.tv_nsec);
#else
    uint64_t modified = timespec_value(info.st_mtim.tv_sec,
                                       info.st_mtim.tv_nsec);
    uint64_t changed = timespec_value(info.st_ctim.tv_sec,
                                      info.st_ctim.tv_nsec);
#endif
    *signature = (file_signature_t){
        .size = (uint64_t)info.st_size,
        .modified = modified,
        .changed = changed,
        .device = (uint64_t)info.st_dev,
        .inode = (uint64_t)info.st_ino,
    };
    return 1;
}
#endif

static bool
read_file(const char *path, char **bytes_out, size_t *size_out)
{
    FILE *file = open_binary(path);
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    char *bytes = core_alloc_size(1, (size_t)length + 1);
    size_t read = fread(bytes, 1, (size_t)length, file);
    bool ok = !ferror(file) && read == (size_t)length;
    fclose(file);
    if (!ok) {
        core_free(bytes);
        return false;
    }
    *bytes_out = bytes;
    *size_out = read;
    return true;
}

core_source_snapshot_t *
core_source_snapshot_retain(const core_source_snapshot_t *snapshot)
{
    if (!snapshot) return nullptr;
    core_source_snapshot_t *owned = (core_source_snapshot_t *)snapshot;
    owned->references++;
    return owned;
}

void
core_source_snapshot_release(core_source_snapshot_t *snapshot)
{
    if (!snapshot || --snapshot->references) return;
    core_source_file_release(&snapshot->source);
    core_free(snapshot);
}

core_string_t
core_source_snapshot_path(const core_source_snapshot_t *snapshot)
{
    return snapshot ? core_source_file_path(&snapshot->source)
                    : (core_string_t){0};
}

const char *
core_source_snapshot_contents(const core_source_snapshot_t *snapshot)
{
    return snapshot ? core_source_file_contents(&snapshot->source) : nullptr;
}

size_t
core_source_snapshot_size(const core_source_snapshot_t *snapshot)
{
    return snapshot ? core_source_file_size(&snapshot->source) : 0;
}

uint64_t
core_source_snapshot_revision(const core_source_snapshot_t *snapshot)
{
    return snapshot ? snapshot->revision : 0;
}

static void
release_entries(core_list_t(inventory_entry_t) *entries)
{
    for (size_t i = 0; i < entries->len; i++) {
        core_free(entries->data[i].relative.data);
        core_source_snapshot_release(entries->data[i].snapshot);
    }
    core_list_free(*entries);
}

core_snapshot_inventory_t *
core_snapshot_inventory_new(const core_string_t *roots, size_t root_count,
                            core_snapshot_inventory_accept_fn accept,
                            void *context)
{
    if (!roots && root_count) return nullptr;
    core_snapshot_inventory_t *inventory = core_alloc(core_snapshot_inventory_t);
    inventory->roots = core_list_new(core_string_t);
    inventory->entries = core_list_new(inventory_entry_t);
    inventory->accept = accept;
    inventory->context = context;
    for (size_t i = 0; i < root_count; i++) {
        if (!roots[i].data || !roots[i].u8_bytes) {
            core_snapshot_inventory_free(inventory);
            return nullptr;
        }
        core_list_push(inventory->roots, core_string_dup(roots[i]));
    }
    return inventory;
}

void
core_snapshot_inventory_free(core_snapshot_inventory_t *inventory)
{
    if (!inventory) return;
    release_entries(&inventory->entries);
    for (size_t i = 0; i < inventory->roots.len; i++)
        core_free(inventory->roots.data[i].data);
    core_list_free(inventory->roots);
    core_free(inventory->metadata_directory.data);
    core_free(inventory);
}

bool
core_snapshot_inventory_enable_local_cache(core_snapshot_inventory_t *inventory,
                                           core_string_t metadata_directory,
                                           uint64_t filter_revision)
{
#ifdef _WIN32
    (void)inventory;
    (void)metadata_directory;
    (void)filter_revision;
    return false;
#else
    if (!inventory || !metadata_directory.data || !metadata_directory.u8_bytes
        || memchr(metadata_directory.data, '/', metadata_directory.u8_bytes)
        || memchr(metadata_directory.data, '\\', metadata_directory.u8_bytes)
        || memchr(metadata_directory.data, '\0', metadata_directory.u8_bytes)
        || (metadata_directory.u8_bytes == 1
            && metadata_directory.data[0] == '.')) return false;
    core_string_t name = core_string_dup(metadata_directory);
    if (!name.data) return false;
    core_free(inventory->metadata_directory.data);
    inventory->metadata_directory = name;
    inventory->filter_revision = filter_revision;
    return true;
#endif
}

typedef struct {
    core_snapshot_inventory_t *inventory;
    core_list_t(inventory_entry_t) entries;
    bool changed;
} refresh_context_t;

static const inventory_entry_t *
old_entry(const refresh_context_t *refresh, size_t root_index,
          core_string_t relative)
{
    const core_list_t(inventory_entry_t) *entries =
        &refresh->inventory->entries;
    for (size_t i = 0; i < entries->len; i++) {
        const inventory_entry_t *entry = &entries->data[i];
        if (entry->root_index == root_index
            && core_string_eq(entry->relative, relative)) return entry;
    }
    return nullptr;
}

static bool
add_file(refresh_context_t *refresh, size_t root_index,
         const char *relative, const char *path, file_signature_t signature)
{
    core_string_t name = core_string_from_cstr(relative);
    const inventory_entry_t *old = old_entry(refresh, root_index, name);
    core_source_snapshot_t *snapshot = nullptr;
    if (old && !memcmp(&old->signature, &signature, sizeof(signature)))
        snapshot = core_source_snapshot_retain(old->snapshot);
    else {
        char *bytes = nullptr;
        size_t size = 0;
        file_signature_t after = {0};
        bool directory = false, regular = false;
        bool stable = false;
        for (unsigned attempt = 0; attempt < 3; attempt++) {
            refresh->inventory->stats.source_reads++;
            if (!read_file(path, &bytes, &size)) break;
            int found = path_signature(path, &after, &directory, &regular);
            stable = found == 1 && regular
                && !memcmp(&signature, &after, sizeof(signature));
            if (stable) break;
            core_free(bytes);
            bytes = nullptr;
            if (found != 1 || !regular) break;
            signature = after;
        }
        if (!stable) {
            core_free(bytes);
            core_free(name.data);
            return false;
        }
        if (old && core_source_snapshot_size(old->snapshot) == size
            && !memcmp(core_source_snapshot_contents(old->snapshot),
                       bytes, size))
            snapshot = core_source_snapshot_retain(old->snapshot);
        else {
            snapshot = core_alloc(core_source_snapshot_t);
            snapshot->references = 1;
            snapshot->revision = ++refresh->inventory->next_revision;
            if (!core_source_file_init(&snapshot->source,
                    core_string_borrow(path, strlen(path)), bytes, size)) {
                core_free(snapshot);
                core_free(bytes);
                core_free(name.data);
                return false;
            }
            refresh->changed = true;
        }
        core_free(bytes);
    }
    core_list_push(refresh->entries, ((inventory_entry_t){
        .root_index = root_index,
        .relative = name,
        .signature = signature,
        .snapshot = snapshot,
    }));
    return true;
}

static bool
scan_directory(refresh_context_t *refresh, size_t root_index,
               const char *relative)
{
    const char *root = refresh->inventory->roots.data[root_index].data;
    char *path = *relative ? join_path(root, relative) : core_strdup(root);
#ifdef _WIN32
    char *pattern = join_path(path, "*");
    wchar_t *wide = wide_path(pattern);
    WIN32_FIND_DATAW item;
    HANDLE search = wide ? FindFirstFileW(wide, &item) : INVALID_HANDLE_VALUE;
    DWORD error = wide && search == INVALID_HANDLE_VALUE
        ? GetLastError() : ERROR_INVALID_NAME;
    core_free(wide);
    core_free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        core_free(path);
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    bool ok = true;
    do {
        if (!wcscmp(item.cFileName, L".")
            || !wcscmp(item.cFileName, L"..")) continue;
        char *name = utf8_name(item.cFileName);
        if (!name) { ok = false; break; }
        char *child = *relative ? join_path(relative, name)
                                : core_strdup(name);
        char *absolute = join_path(root, child);
        file_signature_t signature = {0};
        bool directory = false, regular = false;
        int found = path_signature(absolute, &signature, &directory, &regular);
        if (found != 1) ok = false;
        else if (directory) ok = scan_directory(refresh, root_index, child);
        else if (regular && (!refresh->inventory->accept
            || refresh->inventory->accept(refresh->inventory->context,
                                         core_string_borrow(child,
                                                            strlen(child)))))
            ok = add_file(refresh, root_index, child, absolute, signature);
        core_free(absolute);
        core_free(child);
        core_free(name);
    } while (ok && FindNextFileW(search, &item));
    FindClose(search);
#else
    DIR *directory = opendir(path);
    if (!directory) {
        int error = errno;
        core_free(path);
        return error == ENOENT || error == ENOTDIR;
    }
    bool ok = true;
    struct dirent *item;
    while (ok && (item = readdir(directory))) {
        if (!strcmp(item->d_name, ".")
            || !strcmp(item->d_name, "..")) continue;
        char *child = *relative ? join_path(relative, item->d_name)
                                : core_strdup(item->d_name);
        char *absolute = join_path(root, child);
        file_signature_t signature = {0};
        bool is_directory = false, regular = false;
        int found = path_signature(absolute, &signature, &is_directory,
                                   &regular);
        if (found < 0) ok = false;
        else if (is_directory) ok = scan_directory(refresh, root_index, child);
        else if (regular && (!refresh->inventory->accept
            || refresh->inventory->accept(refresh->inventory->context,
                                         core_string_borrow(child,
                                                            strlen(child)))))
            ok = add_file(refresh, root_index, child, absolute, signature);
        core_free(absolute);
        core_free(child);
    }
    closedir(directory);
#endif
    core_free(path);
    return ok;
}

#ifndef _WIN32

typedef struct {
    core_string_t relative;
    file_signature_t signature;
} inventory_directory_t;
core_list_decl(inventory_directory_t);

typedef struct {
    const char *relative;
    size_t length;
    file_signature_t signature;
} cached_directory_t;
core_list_decl(cached_directory_t);

typedef struct {
    const char *relative;
    size_t length;
    const char *contents;
    size_t size;
    file_signature_t signature;
} cached_file_t;
core_list_decl(cached_file_t);

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} inventory_reader_t;

static void
release_directories(core_list_t(inventory_directory_t) *directories)
{
    for (size_t i = 0; i < directories->len; i++)
        core_free(directories->data[i].relative.data);
    core_list_free(*directories);
}

static bool
cache_take(inventory_reader_t *reader, size_t length, const char **out)
{
    if (length > reader->size - reader->offset) return false;
    *out = (const char *)reader->data + reader->offset;
    reader->offset += length;
    return true;
}

static bool
cache_u32(inventory_reader_t *reader, uint32_t *out)
{
    const char *bytes;
    if (!cache_take(reader, 4, &bytes)) return false;
    *out = core_le_load_u32((const uint8_t *)bytes);
    return true;
}

static bool
cache_u64(inventory_reader_t *reader, uint64_t *out)
{
    const char *bytes;
    if (!cache_take(reader, 8, &bytes)) return false;
    *out = core_le_load_u64((const uint8_t *)bytes);
    return true;
}

static bool
cache_signature(inventory_reader_t *reader, file_signature_t *out)
{
    return cache_u64(reader, &out->size)
        && cache_u64(reader, &out->modified)
        && cache_u64(reader, &out->changed)
        && cache_u64(reader, &out->device)
        && cache_u64(reader, &out->inode);
}

static void
cache_put_u32(core_buffer_t *buffer, uint32_t value)
{
    uint8_t bytes[4];
    core_le_store_u32(bytes, value);
    core_buffer_append(buffer, (const char *)bytes, sizeof bytes);
}

static void
cache_put_u64(core_buffer_t *buffer, uint64_t value)
{
    uint8_t bytes[8];
    core_le_store_u64(bytes, value);
    core_buffer_append(buffer, (const char *)bytes, sizeof bytes);
}

static void
cache_put_signature(core_buffer_t *buffer, file_signature_t signature)
{
    cache_put_u64(buffer, signature.size);
    cache_put_u64(buffer, signature.modified);
    cache_put_u64(buffer, signature.changed);
    cache_put_u64(buffer, signature.device);
    cache_put_u64(buffer, signature.inode);
}

static bool
safe_relative_path(const char *path, size_t length,
                   const char *group, bool recursive)
{
    if (memchr(path, '\0', length) || memchr(path, '\\', length)) return false;
    if (!length) return !*group;
    if (path[0] == '/' || path[length - 1] == '/') return false;
    if (*group) {
        size_t prefix = strlen(group);
        if (length < prefix || memcmp(path, group, prefix)) return false;
        if (length != prefix && path[prefix] != '/') return false;
    }
    else if (!recursive && memchr(path, '/', length)) return false;
    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && path[end] != '/') end++;
        if (end == start || (end - start == 1 && path[start] == '.')
            || (end - start == 2 && path[start] == '.'
                && path[start + 1] == '.')) return false;
        start = end + 1;
    }
    return true;
}

static char *
cache_file_path(const core_snapshot_inventory_t *inventory,
                const char *group_path)
{
    char *metadata = join_path(group_path,
        inventory->metadata_directory.data);
    char *path = join_path(metadata, "inventory-v1");
    core_free(metadata);
    return path;
}

static bool
same_path_signature(const char *path, file_signature_t expected,
                    bool want_directory)
{
    file_signature_t actual = {0};
    bool directory = false, regular = false;
    return path_signature(path, &actual, &directory, &regular) == 1
        && (want_directory ? directory : regular)
        && !memcmp(&actual, &expected, sizeof actual);
}

static bool
add_cached_file(refresh_context_t *refresh, size_t root_index,
                const cached_file_t *cached)
{
    core_string_t relative = core_string_from_raw(cached->relative,
                                                   (int64_t)cached->length);
    if (!relative.data) return false;
    const inventory_entry_t *old = old_entry(refresh, root_index, relative);
    core_source_snapshot_t *snapshot = nullptr;
    if (old && !memcmp(&old->signature, &cached->signature,
                       sizeof cached->signature))
        snapshot = core_source_snapshot_retain(old->snapshot);
    else {
        const char *root = refresh->inventory->roots.data[root_index].data;
        char *path = join_path(root, relative.data);
        snapshot = core_alloc(core_source_snapshot_t);
        snapshot->references = 1;
        snapshot->revision = ++refresh->inventory->next_revision;
        bool ok = core_source_file_init(&snapshot->source,
            core_string_borrow(path, strlen(path)), cached->contents,
            cached->size);
        core_free(path);
        if (!ok) {
            core_free(snapshot);
            core_free(relative.data);
            return false;
        }
        refresh->changed = true;
    }
    core_list_push(refresh->entries, ((inventory_entry_t){
        .root_index = root_index,
        .relative = relative,
        .signature = cached->signature,
        .snapshot = snapshot,
    }));
    return true;
}

static bool
load_package_cache(refresh_context_t *refresh, size_t root_index,
                   const char *group, bool recursive, const char *group_path)
{
    core_snapshot_inventory_t *inventory = refresh->inventory;
    char *path = cache_file_path(inventory, group_path);
    struct stat status;
    bool plausible = !stat(path, &status) && S_ISREG(status.st_mode)
        && status.st_size >= 40 && status.st_size <= 256 * 1024 * 1024;
    char *bytes = nullptr;
    size_t size = 0;
    bool ok = plausible && read_file(path, &bytes, &size);
    core_free(path);
    if (!ok) return false;
    const uint8_t *raw = (const uint8_t *)bytes;
    uint64_t content_hash = core_le_load_u64(raw + 8);
    uint64_t path_hash = core_le_load_u64(raw + 16);
    uint64_t filter_revision = core_le_load_u64(raw + 24);
    uint32_t directory_count = core_le_load_u32(raw + 32);
    uint32_t file_count = core_le_load_u32(raw + 36);
    ok = !memcmp(raw, "LNSINV2", 8)
        && content_hash == core_xxh3_64bits_raw(raw + 40, size - 40)
        && path_hash == core_xxh3_64bits_raw(group_path,
                                             strlen(group_path))
        && filter_revision == inventory->filter_revision
        && directory_count <= 100000 && file_count <= 100000;
    inventory_reader_t reader = {.data = raw, .size = size, .offset = 40};
    core_list_t(cached_directory_t) directories =
        core_list_new(cached_directory_t);
    core_list_t(cached_file_t) files = core_list_new(cached_file_t);
    for (uint32_t i = 0; ok && i < directory_count; i++) {
        uint32_t length = 0;
        file_signature_t signature = {0};
        const char *relative = nullptr;
        ok = cache_u32(&reader, &length)
            && cache_signature(&reader, &signature)
            && cache_take(&reader, length, &relative)
            && safe_relative_path(relative, length, group, recursive);
        if (ok) core_list_push(directories, ((cached_directory_t){
            .relative = relative, .length = length, .signature = signature,
        }));
    }
    for (uint32_t i = 0; ok && i < file_count; i++) {
        uint32_t length = 0;
        uint64_t content_size = 0;
        file_signature_t signature = {0};
        const char *relative = nullptr, *contents = nullptr;
        ok = cache_u32(&reader, &length)
            && cache_signature(&reader, &signature)
            && cache_u64(&reader, &content_size)
            && content_size <= SIZE_MAX
            && cache_take(&reader, length, &relative)
            && cache_take(&reader, (size_t)content_size, &contents)
            && safe_relative_path(relative, length, group, recursive)
            && (!inventory->accept
                || inventory->accept(inventory->context,
                    core_string_borrow(relative, length)));
        if (ok) core_list_push(files, ((cached_file_t){
            .relative = relative, .length = length,
            .contents = contents, .size = (size_t)content_size,
            .signature = signature,
        }));
    }
    ok = ok && reader.offset == size && directories.len > 0;
    const char *root = inventory->roots.data[root_index].data;
    for (size_t i = 0; ok && i < directories.len; i++) {
        cached_directory_t *record = &directories.data[i];
        char *relative = core_strndup(record->relative, record->length);
        char *absolute = record->length
            ? join_path(root, relative) : core_strdup(root);
        ok = same_path_signature(absolute, record->signature, true);
        core_free(absolute);
        core_free(relative);
    }
    for (size_t i = 0; ok && i < files.len; i++) {
        cached_file_t *record = &files.data[i];
        char *relative = core_strndup(record->relative, record->length);
        char *absolute = join_path(root, relative);
        ok = same_path_signature(absolute, record->signature, false);
        core_free(absolute);
        core_free(relative);
    }
    if (ok) for (size_t i = 0; i < files.len; i++)
        if (!add_cached_file(refresh, root_index, &files.data[i])) {
            ok = false;
            break;
        }
    core_list_free(directories);
    core_list_free(files);
    core_free(bytes);
    return ok;
}

static bool
scan_package_directory(refresh_context_t *refresh, size_t root_index,
                       const char *relative, bool recursive,
                       core_list_t(inventory_directory_t) *directories)
{
    const char *root = refresh->inventory->roots.data[root_index].data;
    char *path = *relative ? join_path(root, relative) : core_strdup(root);
    file_signature_t signature = {0};
    bool is_directory = false, regular = false;
    int found = path_signature(path, &signature, &is_directory, &regular);
    if (found != 1 || !is_directory) {
        core_free(path);
        return found == 0;
    }
    core_list_push(*directories, ((inventory_directory_t){
        .relative = core_string_from_cstr(relative),
        .signature = signature,
    }));
    DIR *directory = opendir(path);
    if (!directory) {
        core_free(path);
        return false;
    }
    bool ok = true;
    struct dirent *item;
    while (ok && (item = readdir(directory))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")
            || item->d_name[0] == '.') continue;
        char *child = *relative ? join_path(relative, item->d_name)
                                : core_strdup(item->d_name);
        char *absolute = join_path(root, child);
        file_signature_t child_signature = {0};
        bool child_directory = false, child_regular = false;
        int child_found = path_signature(absolute, &child_signature,
                                        &child_directory, &child_regular);
        if (child_found < 0) ok = false;
        else if (child_directory && recursive)
            ok = scan_package_directory(refresh, root_index, child,
                                        true, directories);
        else if (child_regular && (!refresh->inventory->accept
            || refresh->inventory->accept(refresh->inventory->context,
                                         core_string_borrow(child,
                                                            strlen(child)))))
            ok = add_file(refresh, root_index, child, absolute,
                          child_signature);
        core_free(absolute);
        core_free(child);
    }
    closedir(directory);
    core_free(path);
    return ok;
}

static bool
write_atomic_cache_file(const char *directory, const char *name,
                        const void *data, size_t size)
{
    char *path = join_path(directory, name);
    core_buffer_t pattern = {0};
    core_buffer_printf(&pattern, "%s/%s.tmp.XXXXXX", directory, name);
    int fd = pattern.data ? mkstemp(pattern.data) : -1;
    bool ok = fd >= 0;
    if (ok) ok = fchmod(fd, 0600) == 0;
    FILE *file = ok ? fdopen(fd, "wb") : nullptr;
    if (ok && !file) ok = false;
    if (ok) {
        ok = fwrite(data, 1, size, file) == size;
        if (ok) ok = fflush(file) == 0;
        if (ok) ok = fsync(fileno(file)) == 0;
        if (fclose(file)) ok = false;
    }
    else if (fd >= 0) close(fd);
    if (ok) ok = rename(pattern.data, path) == 0;
    if (!ok && pattern.data) unlink(pattern.data);
    core_free(path);
    core_buffer_release(&pattern);
    return ok;
}

static bool
write_package_cache(refresh_context_t *refresh,
                    const char *group_path,
                    core_list_t(inventory_directory_t) *directories,
                    size_t first_entry)
{
    core_snapshot_inventory_t *inventory = refresh->inventory;
    char *metadata = join_path(group_path,
        inventory->metadata_directory.data);
    if (mkdir(metadata, 0700) != 0 && errno != EEXIST) {
        core_free(metadata);
        return false;
    }
    struct stat metadata_status;
    if (lstat(metadata, &metadata_status) != 0
        || !S_ISDIR(metadata_status.st_mode)) {
        core_free(metadata);
        return false;
    }
    file_signature_t current = {0};
    bool is_directory = false, regular = false;
    if (!directories->len
        || path_signature(group_path, &current, &is_directory, &regular) != 1
        || !is_directory) {
        core_free(metadata);
        return false;
    }
    directories->data[0].signature = current;
    if (directories->len > UINT32_MAX
        || refresh->entries.len - first_entry > UINT32_MAX) {
        core_free(metadata);
        return false;
    }
    core_buffer_t buffer = {0};
    core_buffer_append(&buffer, "LNSINV2", 8);
    cache_put_u64(&buffer, 0);
    cache_put_u64(&buffer,
        core_xxh3_64bits_raw(group_path, strlen(group_path)));
    cache_put_u64(&buffer, inventory->filter_revision);
    cache_put_u32(&buffer, (uint32_t)directories->len);
    cache_put_u32(&buffer, (uint32_t)(refresh->entries.len - first_entry));
    for (size_t i = 0; i < directories->len; i++) {
        inventory_directory_t *dir = &directories->data[i];
        cache_put_u32(&buffer, (uint32_t)dir->relative.u8_bytes);
        cache_put_signature(&buffer, dir->signature);
        core_buffer_append(&buffer, dir->relative.data,
                           dir->relative.u8_bytes);
    }
    for (size_t i = first_entry; i < refresh->entries.len; i++) {
        inventory_entry_t *entry = &refresh->entries.data[i];
        size_t size = core_source_snapshot_size(entry->snapshot);
        cache_put_u32(&buffer, (uint32_t)entry->relative.u8_bytes);
        cache_put_signature(&buffer, entry->signature);
        cache_put_u64(&buffer, size);
        core_buffer_append(&buffer, entry->relative.data,
                           entry->relative.u8_bytes);
        core_buffer_append(&buffer,
            core_source_snapshot_contents(entry->snapshot), size);
    }
    uint8_t checksum[8];
    core_le_store_u64(checksum,
        core_xxh3_64bits_raw(buffer.data + 40, buffer.byte_len - 40));
    memcpy(buffer.data + 8, checksum, 8);
    bool ok = write_atomic_cache_file(metadata, "inventory-v1",
                                      buffer.data, buffer.byte_len);
    core_free(metadata);
    core_buffer_release(&buffer);
    return ok;
}

static bool
refresh_package_group(refresh_context_t *refresh, size_t root_index,
                      const char *group, bool recursive)
{
    const char *root = refresh->inventory->roots.data[root_index].data;
    char *group_path = *group ? join_path(root, group) : core_strdup(root);
    size_t first_entry = refresh->entries.len;
    if (load_package_cache(refresh, root_index, group, recursive,
                           group_path)) {
        refresh->inventory->stats.package_cache_hits++;
        core_free(group_path);
        return true;
    }
    refresh->inventory->stats.package_scans++;
    /* Invalid metadata is disposable. A failed load may have appended entries
     * only after validation, so discard that partial group before scanning. */
    while (refresh->entries.len > first_entry) {
        inventory_entry_t *entry = &refresh->entries.data[--refresh->entries.len];
        core_free(entry->relative.data);
        core_source_snapshot_release(entry->snapshot);
    }
    core_list_t(inventory_directory_t) directories =
        core_list_new(inventory_directory_t);
    bool ok = scan_package_directory(refresh, root_index, group, recursive,
                                     &directories);
    char *path = cache_file_path(refresh->inventory, group_path);
    bool had_cache = access(path, F_OK) == 0;
    core_free(path);
    if (ok && (refresh->entries.len > first_entry || had_cache))
        (void)write_package_cache(refresh, group_path,
                                  &directories, first_entry);
    release_directories(&directories);
    core_free(group_path);
    return ok;
}

static bool
refresh_local_packages(refresh_context_t *refresh, size_t root_index)
{
    const char *root = refresh->inventory->roots.data[root_index].data;
    if (!refresh_package_group(refresh, root_index, "", false)) return false;
    DIR *directory = opendir(root);
    if (!directory) return errno == ENOENT || errno == ENOTDIR;
    bool ok = true;
    struct dirent *item;
    while (ok && (item = readdir(directory))) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")
            || item->d_name[0] == '.') continue;
        char *path = join_path(root, item->d_name);
        file_signature_t signature = {0};
        bool is_directory = false, regular = false;
        int found = path_signature(path, &signature, &is_directory, &regular);
        if (found < 0) ok = false;
        else if (is_directory)
            ok = refresh_package_group(refresh, root_index,
                                       item->d_name, true);
        core_free(path);
    }
    closedir(directory);
    return ok;
}

static const inventory_entry_t *
product_source(const core_snapshot_inventory_t *inventory, size_t root_index,
               core_string_t relative)
{
    if (!inventory || !inventory->metadata_directory.data) return nullptr;
    for (size_t i = 0; i < inventory->entries.len; i++) {
        const inventory_entry_t *entry = &inventory->entries.data[i];
        if (entry->root_index == root_index
            && core_string_eq(entry->relative, relative)) return entry;
    }
    return nullptr;
}

static char *
product_directory(const core_snapshot_inventory_t *inventory,
                  size_t root_index, core_string_t relative)
{
    if (root_index >= inventory->roots.len) return nullptr;
    const char *root = inventory->roots.data[root_index].data;
    const char *slash = memchr(relative.data, '/', relative.u8_bytes);
    char *group = slash
        ? core_strndup(relative.data, (size_t)(slash - relative.data))
        : nullptr;
    char *package = group ? join_path(root, group) : core_strdup(root);
    char *metadata = join_path(package, inventory->metadata_directory.data);
    char *directory = join_path(metadata, "derived");
    core_free(group);
    core_free(package);
    core_free(metadata);
    return directory;
}

static void
product_name(core_buffer_t *name, core_string_t relative,
             core_string_t kind, uint64_t profile)
{
    uint64_t source_hash = core_xxh3_64bits_raw(relative.data,
                                                relative.u8_bytes);
    uint64_t kind_hash = core_xxh3_64bits_raw(kind.data, kind.u8_bytes);
    core_buffer_printf(name, "%016llx-%016llx-%016llx.bin",
        (unsigned long long)kind_hash,
        (unsigned long long)source_hash,
        (unsigned long long)profile);
}

bool
core_snapshot_inventory_product_read(
    const core_snapshot_inventory_t *inventory, size_t root_index,
    core_string_t relative_path, core_string_t kind, uint64_t profile,
    core_buffer_t *out)
{
    const inventory_entry_t *entry = product_source(inventory, root_index,
                                                     relative_path);
    if (!entry || !kind.data || !kind.u8_bytes || !out) return false;
    char *directory = product_directory(inventory, root_index, relative_path);
    core_buffer_t name = {0};
    product_name(&name, relative_path, kind, profile);
    char *path = join_path(directory, name.data);
    core_buffer_release(&name);
    core_free(directory);
    struct stat status;
    bool plausible = !stat(path, &status) && S_ISREG(status.st_mode)
        && status.st_size >= 88 && status.st_size <= 64 * 1024 * 1024;
    char *bytes = nullptr;
    size_t size = 0;
    bool ok = plausible && read_file(path, &bytes, &size);
    core_free(path);
    if (!ok) return false;
    const uint8_t *raw = (const uint8_t *)bytes;
    uint64_t relative_hash = core_xxh3_64bits_raw(relative_path.data,
                                                  relative_path.u8_bytes);
    uint64_t kind_hash = core_xxh3_64bits_raw(kind.data, kind.u8_bytes);
    inventory_reader_t reader = {.data = raw, .size = size, .offset = 8};
    uint64_t stored_relative = 0, stored_kind = 0, stored_profile = 0;
    uint64_t payload_size = 0, payload_hash = 0;
    file_signature_t signature = {0};
    ok = !memcmp(raw, "LNSDRV1", 8)
        && cache_u64(&reader, &stored_relative)
        && cache_u64(&reader, &stored_kind)
        && cache_u64(&reader, &stored_profile)
        && cache_signature(&reader, &signature)
        && cache_u64(&reader, &payload_size)
        && cache_u64(&reader, &payload_hash)
        && reader.offset == 88
        && stored_relative == relative_hash && stored_kind == kind_hash
        && stored_profile == profile
        && !memcmp(&signature, &entry->signature, sizeof signature)
        && payload_size == size - 88
        && payload_hash == core_xxh3_64bits_raw(raw + 88, size - 88);
    if (ok) core_buffer_append(out, bytes + 88, size - 88);
    core_free(bytes);
    return ok;
}

bool
core_snapshot_inventory_product_write(
    const core_snapshot_inventory_t *inventory, size_t root_index,
    core_string_t relative_path, core_string_t kind, uint64_t profile,
    const void *data, size_t size)
{
    const inventory_entry_t *entry = product_source(inventory, root_index,
                                                     relative_path);
    if (!entry || !kind.data || !kind.u8_bytes || (!data && size)
        || size > 64 * 1024 * 1024) return false;
    char *directory = product_directory(inventory, root_index, relative_path);
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
        core_free(directory);
        return false;
    }
    struct stat status;
    if (lstat(directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
        core_free(directory);
        return false;
    }
    core_buffer_t name = {0};
    product_name(&name, relative_path, kind, profile);
    core_buffer_t buffer = {0};
    core_buffer_append(&buffer, "LNSDRV1", 8);
    cache_put_u64(&buffer, core_xxh3_64bits_raw(relative_path.data,
                                                relative_path.u8_bytes));
    cache_put_u64(&buffer, core_xxh3_64bits_raw(kind.data, kind.u8_bytes));
    cache_put_u64(&buffer, profile);
    cache_put_signature(&buffer, entry->signature);
    cache_put_u64(&buffer, size);
    cache_put_u64(&buffer, core_xxh3_64bits_raw(data ? data : "", size));
    core_buffer_append(&buffer, data, size);
    bool ok = write_atomic_cache_file(directory, name.data,
                                      buffer.data, buffer.byte_len);
    core_buffer_release(&buffer);
    core_buffer_release(&name);
    core_free(directory);
    return ok;
}

#else
bool
core_snapshot_inventory_product_read(
    const core_snapshot_inventory_t *inventory, size_t root_index,
    core_string_t relative_path, core_string_t kind, uint64_t profile,
    core_buffer_t *out)
{
    (void)inventory; (void)root_index; (void)relative_path;
    (void)kind; (void)profile; (void)out;
    return false;
}

bool
core_snapshot_inventory_product_write(
    const core_snapshot_inventory_t *inventory, size_t root_index,
    core_string_t relative_path, core_string_t kind, uint64_t profile,
    const void *data, size_t size)
{
    (void)inventory; (void)root_index; (void)relative_path;
    (void)kind; (void)profile; (void)data; (void)size;
    return false;
}
#endif

bool
core_snapshot_inventory_refresh(core_snapshot_inventory_t *inventory)
{
    if (!inventory) return false;
    inventory->stats = (core_snapshot_inventory_stats_t){0};
    refresh_context_t refresh = {
        .inventory = inventory,
        .entries = core_list_new(inventory_entry_t),
    };
    bool ok = true;
    for (size_t i = 0; ok && i < inventory->roots.len; i++) {
#ifndef _WIN32
        if (inventory->metadata_directory.data)
            ok = refresh_local_packages(&refresh, i);
        else
#endif
            ok = scan_directory(&refresh, i, "");
    }
    if (!ok) {
        release_entries(&refresh.entries);
        return false;
    }
    if (refresh.entries.len != inventory->entries.len)
        refresh.changed = true;
    release_entries(&inventory->entries);
    inventory->entries = refresh.entries;
    if (refresh.changed) inventory->generation++;
    return true;
}

uint64_t
core_snapshot_inventory_generation(const core_snapshot_inventory_t *inventory)
{
    return inventory ? inventory->generation : 0;
}

core_snapshot_inventory_stats_t
core_snapshot_inventory_stats(const core_snapshot_inventory_t *inventory)
{
    return inventory ? inventory->stats : (core_snapshot_inventory_stats_t){0};
}

size_t
core_snapshot_inventory_count(const core_snapshot_inventory_t *inventory)
{
    return inventory ? inventory->entries.len : 0;
}

const core_source_snapshot_t *
core_snapshot_inventory_find(const core_snapshot_inventory_t *inventory,
                             size_t root_index, core_string_t relative_path)
{
    if (!inventory) return nullptr;
    for (size_t i = 0; i < inventory->entries.len; i++) {
        const inventory_entry_t *entry = &inventory->entries.data[i];
        if (entry->root_index == root_index
            && core_string_eq(entry->relative, relative_path))
            return entry->snapshot;
    }
    return nullptr;
}

bool
core_snapshot_inventory_visit(const core_snapshot_inventory_t *inventory,
                              core_snapshot_inventory_visit_fn visit,
                              void *context)
{
    if (!inventory || !visit) return false;
    for (size_t i = 0; i < inventory->entries.len; i++) {
        const inventory_entry_t *entry = &inventory->entries.data[i];
        if (!visit(context, entry->root_index, entry->relative,
                   entry->snapshot)) return false;
    }
    return true;
}
