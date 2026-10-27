#include "lib/snapshot_inventory.h"

#include <string.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/byteorder.h"
#include "lib/hash.h"
#include "lib/list.h"
#include "lib/os.h"

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
core_list_decl(core_snapshot_root_t);

struct core_snapshot_inventory_t {
    core_list_t(core_snapshot_root_t) roots;
    core_list_t(inventory_entry_t) entries;
    core_snapshot_inventory_accept_fn accept;
    void *context;
    uint64_t generation;
    uint64_t next_revision;
    core_string_t cache_root;
    uint64_t filter_revision;
    core_snapshot_inventory_stats_t stats;
};

static core_string_t
metadata_path(const core_snapshot_inventory_t *inventory,
              core_string_t package)
{
    core_string_t resolved = core_os_real_path(package);
    core_string_t relative = core_os_root_relative(resolved);
    core_string_t path = relative.data
        ? core_os_path_join(inventory->cache_root, relative) : (core_string_t){0};
    core_free(relative.data);
    core_free(resolved.data);
    return path;
}

static bool
make_directories(core_string_t path)
{
    for (size_t at = 1; at < path.u8_bytes; at++)
        if (path.data[at] == '/')
            (void)core_os_make_directory(core_string_slice(path, 0, at),
                                         0700);
    return core_os_make_directory(path, 0700);
}

static bool
is_kind(core_string_t path, bool follow, core_os_kind_t kind)
{
    core_os_file_info_t info = {0};
    return core_os_stat(path, follow, &info) == CORE_OS_FOUND
        && info.kind == kind;
}

static bool
plausible_file(core_string_t path, uint64_t minimum, uint64_t maximum)
{
    core_os_file_info_t info = {0};
    return core_os_stat(path, true, &info) == CORE_OS_FOUND
        && info.kind == CORE_OS_KIND_FILE
        && info.size >= minimum && info.size <= maximum;
}

static bool
visible_name(core_string_t name)
{
    return name.u8_bytes && name.data[0] != '.';
}

static core_os_lookup_t
path_signature(core_string_t path, file_signature_t *signature,
               core_os_kind_t *kind)
{
    core_os_file_info_t info = {0};
    core_os_lookup_t found = core_os_stat(path, false, &info);
    if (found != CORE_OS_FOUND) return found;
    *kind = info.kind;
    *signature = (file_signature_t){
        .size = info.size,
        .modified = info.modified,
        .changed = info.changed,
        .device = info.device,
        .inode = info.inode,
    };
    return found;
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

core_string_t
core_source_snapshot_contents(const core_source_snapshot_t *snapshot)
{
    return snapshot ? core_source_file_contents(&snapshot->source)
                    : (core_string_t){};
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
core_snapshot_inventory_new(const core_snapshot_root_t *roots,
                            size_t root_count,
                            core_snapshot_inventory_accept_fn accept,
                            void *context)
{
    if (!roots && root_count) return nullptr;
    core_snapshot_inventory_t *inventory = core_alloc(core_snapshot_inventory_t);
    inventory->roots = core_list_new(core_snapshot_root_t);
    inventory->entries = core_list_new(inventory_entry_t);
    inventory->accept = accept;
    inventory->context = context;
    for (size_t i = 0; i < root_count; i++) {
        if (!roots[i].path.data || !roots[i].path.u8_bytes) {
            core_snapshot_inventory_free(inventory);
            return nullptr;
        }
        core_list_push(inventory->roots, ((core_snapshot_root_t){
            .path = core_string_dup(roots[i].path),
            .enters_fileless_directories =
                roots[i].enters_fileless_directories,
        }));
    }
    return inventory;
}

void
core_snapshot_inventory_free(core_snapshot_inventory_t *inventory)
{
    if (!inventory) return;
    release_entries(&inventory->entries);
    for (size_t i = 0; i < inventory->roots.len; i++)
        core_free(inventory->roots.data[i].path.data);
    core_list_free(inventory->roots);
    core_free(inventory->cache_root.data);
    core_free(inventory);
}

bool
core_snapshot_inventory_enable_cache(core_snapshot_inventory_t *inventory,
                                     core_string_t cache_root,
                                     uint64_t filter_revision)
{
    if (!inventory || !cache_root.data || !core_os_is_absolute(cache_root)
        || core_string_find_byte(cache_root, '\0') >= 0) return false;
    core_string_t root = core_string_dup(cache_root);
    if (!root.data) return false;
    core_free(inventory->cache_root.data);
    inventory->cache_root = root;
    inventory->filter_revision = filter_revision;
    return true;
}

typedef struct {
    core_string_t relative;
    file_signature_t signature;
} inventory_directory_t;
core_list_decl(inventory_directory_t);

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
accepts(const core_snapshot_inventory_t *inventory, core_string_t relative)
{
    return !inventory->accept || inventory->accept(inventory->context,
                                                   relative);
}

static bool
add_file(refresh_context_t *refresh, size_t root_index,
         core_string_t relative, core_string_t path,
         file_signature_t signature)
{
    core_string_t name = core_string_dup(relative);
    const inventory_entry_t *old = old_entry(refresh, root_index, name);
    core_source_snapshot_t *snapshot = nullptr;
    if (old && !memcmp(&old->signature, &signature, sizeof(signature)))
        snapshot = core_source_snapshot_retain(old->snapshot);
    else {
        char *bytes = nullptr;
        size_t size = 0;
        file_signature_t after = {0};
        core_os_kind_t kind = CORE_OS_KIND_OTHER;
        bool stable = false;
        for (unsigned attempt = 0; attempt < 3; attempt++) {
            refresh->inventory->stats.source_reads++;
            if (!core_os_read_file(path, &bytes, &size)) {
                if (attempt == 0) {
                    core_free(name.data);
                    return true;
                }
                break;
            }
            bool found = path_signature(path, &after, &kind) == CORE_OS_FOUND
                && kind == CORE_OS_KIND_FILE;
            stable = found && !memcmp(&signature, &after, sizeof(signature));
            if (stable) break;
            core_free(bytes);
            bytes = nullptr;
            if (!found) break;
            signature = after;
        }
        if (!stable) {
            core_free(bytes);
            core_free(name.data);
            return false;
        }
        core_string_t previous = old
            ? core_source_snapshot_contents(old->snapshot) : (core_string_t){};
        if (old && previous.u8_bytes == size
            && (!size || !memcmp(previous.data, bytes, size)))
            snapshot = core_source_snapshot_retain(old->snapshot);
        else {
            snapshot = core_alloc(core_source_snapshot_t);
            snapshot->references = 1;
            snapshot->revision = ++refresh->inventory->next_revision;
            if (!core_source_file_init(&snapshot->source, path,
                                       core_string_borrow(bytes, size))) {
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


typedef struct {
    bool files;
    bool sources;
} directory_contents_t;

static directory_contents_t
directory_contents(const core_snapshot_inventory_t *inventory,
                   core_string_t absolute)
{
    directory_contents_t contents = {0};
    core_os_dir_t *dir = core_os_dir_open(absolute, nullptr);
    if (!dir) return contents;
    core_string_t name = {};
    core_os_kind_t kind = CORE_OS_KIND_OTHER;
    while (!contents.sources && core_os_dir_next(dir, &name, &kind)) {
        if (!visible_name(name) || kind != CORE_OS_KIND_FILE) continue;
        contents.files = true;
        contents.sources = accepts(inventory, name);
    }
    core_os_dir_close(dir);
    return contents;
}

static bool
descends(const core_snapshot_inventory_t *inventory, size_t root_index,
         core_string_t absolute)
{
    for (size_t i = 0; i < inventory->roots.len; i++)
        if (core_string_eq(inventory->roots.data[i].path, absolute))
            return false;
    bool fileless = inventory->roots.data[root_index].enters_fileless_directories;
    directory_contents_t contents = directory_contents(inventory, absolute);
    return contents.sources || (fileless && !contents.files);
}

static bool scan_tree(refresh_context_t *refresh, size_t root_index,
                      core_string_t relative, bool recursive,
                      core_list_t(inventory_directory_t) *directories);

static bool
scan_child(refresh_context_t *refresh, size_t root_index,
           core_string_t child,
           core_list_t(inventory_directory_t) *directories)
{
    core_string_t root = refresh->inventory->roots.data[root_index].path;
    core_string_t absolute = core_os_path_join(root, child);
    file_signature_t signature = {0};
    core_os_kind_t kind = CORE_OS_KIND_OTHER;
    core_os_lookup_t found = path_signature(absolute, &signature, &kind);
    bool ok = found != CORE_OS_FAILED;
    if (found == CORE_OS_FOUND && kind == CORE_OS_KIND_DIRECTORY) {
        if (directories) core_list_push(*directories, ((inventory_directory_t){
            .relative = core_string_dup(child),
            .signature = signature,
        }));
        if (descends(refresh->inventory, root_index, absolute))
            ok = scan_tree(refresh, root_index, child, true, directories);
    }
    else if (found == CORE_OS_FOUND && kind == CORE_OS_KIND_FILE)
        ok = add_file(refresh, root_index, child, absolute, signature);
    core_free(absolute.data);
    return ok;
}

static bool
scan_tree(refresh_context_t *refresh, size_t root_index,
          core_string_t relative, bool recursive,
          core_list_t(inventory_directory_t) *directories)
{
    core_string_t root = refresh->inventory->roots.data[root_index].path;
    core_string_t path = relative.u8_bytes ? core_os_path_join(root, relative)
                                           : core_string_dup(root);
    core_os_lookup_t status = CORE_OS_FAILED;
    core_os_dir_t *dir = core_os_dir_open(path, &status);
    core_free(path.data);
    if (!dir) return status == CORE_OS_MISSING;
    bool ok = true;
    core_string_t name = {};
    core_os_kind_t kind = CORE_OS_KIND_OTHER;
    while (ok && core_os_dir_next(dir, &name, &kind)) {
        if (!visible_name(name)) continue;
        core_string_t child = relative.u8_bytes ? core_os_path_join(relative, name)
                                                : core_string_dup(name);
        bool wanted = kind == CORE_OS_KIND_DIRECTORY ? recursive
            : kind == CORE_OS_KIND_FILE && accepts(refresh->inventory, child);
        if (wanted) ok = scan_child(refresh, root_index, child, directories);
        core_free(child.data);
    }
    core_os_dir_close(dir);
    return ok;
}

typedef struct {
    core_string_t relative;
    file_signature_t signature;
} cached_directory_t;
core_list_decl(cached_directory_t);

typedef struct {
    core_string_t relative;
    core_string_t contents;
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
cache_take(inventory_reader_t *reader, size_t length, const uint8_t **out)
{
    if (length > reader->size - reader->offset) return false;
    *out = reader->data + reader->offset;
    reader->offset += length;
    return true;
}

static bool
cache_text(inventory_reader_t *reader, size_t length, core_string_t *out)
{
    const uint8_t *bytes;
    if (!cache_take(reader, length, &bytes)) return false;
    *out = core_string_borrow((const char *)bytes, length);
    return true;
}

static bool
cache_u32(inventory_reader_t *reader, uint32_t *out)
{
    const uint8_t *bytes;
    if (!cache_take(reader, 4, &bytes)) return false;
    *out = core_le_load_u32(bytes);
    return true;
}

static bool
cache_u64(inventory_reader_t *reader, uint64_t *out)
{
    const uint8_t *bytes;
    if (!cache_take(reader, 8, &bytes)) return false;
    *out = core_le_load_u64(bytes);
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
safe_relative_path(core_string_t path, core_string_t group, bool recursive)
{
    size_t length = path.u8_bytes;
    if (length && (memchr(path.data, '\0', length)
                   || memchr(path.data, '\\', length))) return false;
    if (!length) return !group.u8_bytes;
    if (path.data[0] == '/' || path.data[length - 1] == '/') return false;
    if (group.u8_bytes) {
        size_t prefix = group.u8_bytes;
        if (!core_string_starts_with(path, group)) return false;
        if (length != prefix && path.data[prefix] != '/') return false;
    }
    else if (!recursive && memchr(path.data, '/', length)) return false;
    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && path.data[end] != '/') end++;
        if (end == start || (end - start == 1 && path.data[start] == '.')
            || (end - start == 2 && path.data[start] == '.'
                && path.data[start + 1] == '.')) return false;
        start = end + 1;
    }
    return true;
}

static core_string_t
cache_file_path(const core_snapshot_inventory_t *inventory,
                core_string_t group_path)
{
    core_string_t metadata = metadata_path(inventory, group_path);
    if (!metadata.data) return (core_string_t){0};
    core_string_t path = core_os_path_join(metadata,
                                   CORE_STRING_STATIC("inventory-v1"));
    core_free(metadata.data);
    return path;
}

static bool
same_path_signature(core_string_t path, file_signature_t expected,
                    bool want_directory)
{
    file_signature_t actual = {0};
    core_os_kind_t kind = CORE_OS_KIND_OTHER;
    return path_signature(path, &actual, &kind) == CORE_OS_FOUND
        && kind == (want_directory ? CORE_OS_KIND_DIRECTORY
                                   : CORE_OS_KIND_FILE)
        && !memcmp(&actual, &expected, sizeof actual);
}

static bool
add_cached_file(refresh_context_t *refresh, size_t root_index,
                const cached_file_t *cached)
{
    core_string_t relative = core_string_from_raw(
        cached->relative.data, (int64_t)cached->relative.u8_bytes);
    if (!relative.data) return false;
    const inventory_entry_t *old = old_entry(refresh, root_index, relative);
    core_source_snapshot_t *snapshot = nullptr;
    if (old && !memcmp(&old->signature, &cached->signature,
                       sizeof cached->signature))
        snapshot = core_source_snapshot_retain(old->snapshot);
    else {
        core_string_t root = refresh->inventory->roots.data[root_index].path;
        core_string_t path = core_os_path_join(root, relative);
        snapshot = core_alloc(core_source_snapshot_t);
        snapshot->references = 1;
        snapshot->revision = ++refresh->inventory->next_revision;
        bool ok = core_source_file_init(&snapshot->source, path,
                                        cached->contents);
        core_free(path.data);
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
                   core_string_t group, bool recursive,
                   core_string_t group_path)
{
    core_snapshot_inventory_t *inventory = refresh->inventory;
    core_string_t path = cache_file_path(inventory, group_path);
    if (!path.data) return false;
    char *bytes = nullptr;
    size_t size = 0;
    bool ok = plausible_file(path, 40, 256 * 1024 * 1024)
        && core_os_read_file(path, &bytes, &size) && size >= 40;
    core_free(path.data);
    if (!ok) return false;
    const uint8_t *raw = (const uint8_t *)bytes;
    uint64_t content_hash = core_le_load_u64(raw + 8);
    uint64_t path_hash = core_le_load_u64(raw + 16);
    uint64_t filter_revision = core_le_load_u64(raw + 24);
    uint32_t directory_count = core_le_load_u32(raw + 32);
    uint32_t file_count = core_le_load_u32(raw + 36);
    ok = !memcmp(raw, "LNSINV2", 8)
        && content_hash == core_xxh3_64bits_raw(raw + 40, size - 40)
        && path_hash == core_xxh3_64bits_raw(group_path.data,
                                             group_path.u8_bytes)
        && filter_revision == inventory->filter_revision
        && directory_count <= 100000 && file_count <= 100000;
    inventory_reader_t reader = {.data = raw, .size = size, .offset = 40};
    core_list_t(cached_directory_t) directories =
        core_list_new(cached_directory_t);
    core_list_t(cached_file_t) files = core_list_new(cached_file_t);
    for (uint32_t i = 0; ok && i < directory_count; i++) {
        uint32_t length = 0;
        file_signature_t signature = {0};
        core_string_t relative = {};
        ok = cache_u32(&reader, &length)
            && cache_signature(&reader, &signature)
            && cache_text(&reader, length, &relative)
            && safe_relative_path(relative, group, recursive);
        if (ok) core_list_push(directories, ((cached_directory_t){
            .relative = relative, .signature = signature,
        }));
    }
    for (uint32_t i = 0; ok && i < file_count; i++) {
        uint32_t length = 0;
        uint64_t content_size = 0;
        file_signature_t signature = {0};
        core_string_t relative = {}, contents = {};
        ok = cache_u32(&reader, &length)
            && cache_signature(&reader, &signature)
            && cache_u64(&reader, &content_size)
            && content_size <= SIZE_MAX
            && cache_text(&reader, length, &relative)
            && cache_text(&reader, (size_t)content_size, &contents)
            && safe_relative_path(relative, group, recursive)
            && accepts(inventory, relative);
        if (ok) core_list_push(files, ((cached_file_t){
            .relative = relative, .contents = contents,
            .signature = signature,
        }));
    }
    ok = ok && reader.offset == size && directories.len > 0;
    core_string_t root = inventory->roots.data[root_index].path;
    for (size_t i = 0; ok && i < directories.len; i++) {
        cached_directory_t *record = &directories.data[i];
        core_string_t absolute = record->relative.u8_bytes
            ? core_os_path_join(root, record->relative) : core_string_dup(root);
        ok = same_path_signature(absolute, record->signature, true);
        core_free(absolute.data);
    }
    for (size_t i = 0; ok && i < files.len; i++) {
        cached_file_t *record = &files.data[i];
        core_string_t absolute = core_os_path_join(root, record->relative);
        ok = same_path_signature(absolute, record->signature, false);
        core_free(absolute.data);
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
write_atomic_cache_file(core_string_t directory, core_string_t name,
                        const void *data, size_t size)
{
    core_buffer_t stem = {0};
    core_buffer_append_string(&stem, directory);
    core_buffer_append(&stem, "/", 1);
    core_buffer_append_string(&stem, name);
    core_buffer_append(&stem, ".tmp.", 5);
    core_string_t temporary = {};
    int fd = core_os_make_unique_file(core_buffer_string(&stem), &temporary);
    core_buffer_release(&stem);
    bool ok = fd >= 0 && core_os_set_mode(fd, 0600)
        && core_os_write_all(fd, data, size) && core_os_flush(fd);
    if (fd >= 0 && !core_os_close(fd)) ok = false;
    core_string_t path = core_os_path_join(directory, name);
    if (ok) ok = core_os_rename_replace(temporary, path);
    if (!ok && temporary.data) (void)core_os_remove_file(temporary);
    core_free(path.data);
    core_free(temporary.data);
    return ok;
}

static bool
write_package_cache(refresh_context_t *refresh,
                    core_string_t group_path,
                    core_list_t(inventory_directory_t) *directories,
                    size_t first_entry)
{
    core_snapshot_inventory_t *inventory = refresh->inventory;
    core_string_t metadata = metadata_path(inventory, group_path);
    if (!metadata.data) return false;
    if (!make_directories(metadata)
        || !is_kind(metadata, false, CORE_OS_KIND_DIRECTORY)) {
        core_free(metadata.data);
        return false;
    }
    file_signature_t current = {0};
    core_os_kind_t kind = CORE_OS_KIND_OTHER;
    if (!directories->len
        || path_signature(group_path, &current, &kind) != CORE_OS_FOUND
        || kind != CORE_OS_KIND_DIRECTORY) {
        core_free(metadata.data);
        return false;
    }
    directories->data[0].signature = current;
    if (directories->len > UINT32_MAX
        || refresh->entries.len - first_entry > UINT32_MAX) {
        core_free(metadata.data);
        return false;
    }
    core_buffer_t buffer = {0};
    core_buffer_append(&buffer, "LNSINV2", 8);
    cache_put_u64(&buffer, 0);
    cache_put_u64(&buffer,
        core_xxh3_64bits_raw(group_path.data, group_path.u8_bytes));
    cache_put_u64(&buffer, inventory->filter_revision);
    cache_put_u32(&buffer, (uint32_t)directories->len);
    cache_put_u32(&buffer, (uint32_t)(refresh->entries.len - first_entry));
    for (size_t i = 0; i < directories->len; i++) {
        inventory_directory_t *dir = &directories->data[i];
        cache_put_u32(&buffer, (uint32_t)dir->relative.u8_bytes);
        cache_put_signature(&buffer, dir->signature);
        core_buffer_append_string(&buffer, dir->relative);
    }
    for (size_t i = first_entry; i < refresh->entries.len; i++) {
        inventory_entry_t *entry = &refresh->entries.data[i];
        core_string_t contents = core_source_snapshot_contents(
            entry->snapshot);
        cache_put_u32(&buffer, (uint32_t)entry->relative.u8_bytes);
        cache_put_signature(&buffer, entry->signature);
        cache_put_u64(&buffer, contents.u8_bytes);
        core_buffer_append_string(&buffer, entry->relative);
        core_buffer_append_string(&buffer, contents);
    }
    uint8_t checksum[8];
    core_le_store_u64(checksum,
        core_xxh3_64bits_raw(buffer.data + 40, buffer.byte_len - 40));
    memcpy(buffer.data + 8, checksum, 8);
    bool ok = write_atomic_cache_file(metadata,
                                      CORE_STRING_STATIC("inventory-v1"),
                                      buffer.data, buffer.byte_len);
    core_free(metadata.data);
    core_buffer_release(&buffer);
    return ok;
}

static bool
refresh_package_group(refresh_context_t *refresh, size_t root_index,
                      core_string_t group, bool recursive)
{
    core_string_t root = refresh->inventory->roots.data[root_index].path;
    core_string_t group_path = group.u8_bytes ? core_os_path_join(root, group)
                                              : core_string_dup(root);
    size_t first_entry = refresh->entries.len;
    if (load_package_cache(refresh, root_index, group, recursive,
                           group_path)) {
        refresh->inventory->stats.package_cache_hits++;
        core_free(group_path.data);
        return true;
    }
    refresh->inventory->stats.package_scans++;
    while (refresh->entries.len > first_entry) {
        inventory_entry_t *entry = &refresh->entries.data[--refresh->entries.len];
        core_free(entry->relative.data);
        core_source_snapshot_release(entry->snapshot);
    }
    core_list_t(inventory_directory_t) directories =
        core_list_new(inventory_directory_t);
    file_signature_t signature = {0};
    core_os_kind_t kind = CORE_OS_KIND_OTHER;
    core_os_lookup_t found = path_signature(group_path, &signature, &kind);
    bool ok = found == CORE_OS_MISSING;
    if (found == CORE_OS_FOUND && kind == CORE_OS_KIND_DIRECTORY) {
        core_list_push(directories, ((inventory_directory_t){
            .relative = core_string_dup(group),
            .signature = signature,
        }));
        ok = scan_tree(refresh, root_index, group, recursive, &directories);
    }
    core_string_t path = cache_file_path(refresh->inventory, group_path);
    core_os_file_info_t existing = {0};
    bool had_cache = path.data
        && core_os_stat(path, true, &existing) == CORE_OS_FOUND;
    core_free(path.data);
    if (ok && (refresh->entries.len > first_entry || had_cache))
        (void)write_package_cache(refresh, group_path,
                                  &directories, first_entry);
    release_directories(&directories);
    core_free(group_path.data);
    return ok;
}

static bool
refresh_local_packages(refresh_context_t *refresh, size_t root_index)
{
    core_string_t root = refresh->inventory->roots.data[root_index].path;
    if (!refresh_package_group(refresh, root_index, CORE_STRING_STATIC(""),
                               false)) return false;
    core_os_lookup_t status = CORE_OS_FAILED;
    core_os_dir_t *dir = core_os_dir_open(root, &status);
    if (!dir) return status == CORE_OS_MISSING;
    bool ok = true;
    core_string_t name = {};
    core_os_kind_t kind = CORE_OS_KIND_OTHER;
    while (ok && core_os_dir_next(dir, &name, &kind)) {
        if (!visible_name(name) || kind != CORE_OS_KIND_DIRECTORY) continue;
        core_string_t path = core_os_path_join(root, name);
        if (descends(refresh->inventory, root_index, path))
            ok = refresh_package_group(refresh, root_index, name, true);
        core_free(path.data);
    }
    core_os_dir_close(dir);
    return ok;
}

static const inventory_entry_t *
product_source(const core_snapshot_inventory_t *inventory, size_t root_index,
               core_string_t relative)
{
    if (!inventory || !inventory->cache_root.data) return nullptr;
    for (size_t i = 0; i < inventory->entries.len; i++) {
        const inventory_entry_t *entry = &inventory->entries.data[i];
        if (entry->root_index == root_index
            && core_string_eq(entry->relative, relative)) return entry;
    }
    return nullptr;
}

static core_string_t
product_directory(const core_snapshot_inventory_t *inventory,
                  size_t root_index, core_string_t relative)
{
    if (root_index >= inventory->roots.len) return (core_string_t){};
    core_string_t root = inventory->roots.data[root_index].path;
    int64_t slash = core_string_find_byte(relative, '/');
    core_string_t package = slash >= 0
        ? core_os_path_join(root, core_string_slice(relative, 0, (size_t)slash))
        : core_string_dup(root);
    core_string_t metadata = metadata_path(inventory, package);
    if (!metadata.data) {
        core_free(package.data);
        return (core_string_t){0};
    }
    core_string_t directory = core_os_path_join(metadata,
                                        CORE_STRING_STATIC("derived"));
    core_free(package.data);
    core_free(metadata.data);
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
    core_string_t directory = product_directory(inventory, root_index,
                                                relative_path);
    if (!directory.data) return false;
    core_buffer_t name = {0};
    product_name(&name, relative_path, kind, profile);
    core_string_t path = core_os_path_join(directory, core_buffer_string(&name));
    core_buffer_release(&name);
    core_free(directory.data);
    char *bytes = nullptr;
    size_t size = 0;
    bool ok = plausible_file(path, 88, 64 * 1024 * 1024)
        && core_os_read_file(path, &bytes, &size) && size >= 88;
    core_free(path.data);
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
    core_string_t directory = product_directory(inventory, root_index,
                                                relative_path);
    if (!directory.data || !make_directories(directory)
        || !is_kind(directory, false, CORE_OS_KIND_DIRECTORY)) {
        core_free(directory.data);
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
    bool ok = write_atomic_cache_file(directory, core_buffer_string(&name),
                                      buffer.data, buffer.byte_len);
    core_buffer_release(&buffer);
    core_buffer_release(&name);
    core_free(directory.data);
    return ok;
}
static int
entry_order(const void *left, const void *right)
{
    const inventory_entry_t *a = left;
    const inventory_entry_t *b = right;
    if (a->root_index != b->root_index)
        return a->root_index < b->root_index ? -1 : 1;
    return core_string_cmp(a->relative, b->relative);
}

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
    for (size_t i = 0; ok && i < inventory->roots.len; i++)
        ok = inventory->cache_root.data
            ? refresh_local_packages(&refresh, i)
            : scan_tree(&refresh, i, CORE_STRING_STATIC(""), true, nullptr);
    if (!ok) {
        release_entries(&refresh.entries);
        return false;
    }
    core_list_sort(refresh.entries, entry_order);
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
