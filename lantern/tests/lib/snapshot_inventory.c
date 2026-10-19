#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/snapshot_inventory.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

typedef struct {
    const core_source_snapshot_t *source;
    size_t count;
} observed_t;

static bool
accept_source(void *context, core_string_t relative)
{
    (void)context;
    return core_string_ends_with(relative, S(".n"));
}

static bool
observe(void *context, size_t root, core_string_t relative,
        const core_source_snapshot_t *source)
{
    observed_t *result = context;
    result->count++;
    if (root == 0 && core_string_eq(relative, S("pkg/manifest.n")))
        result->source = source;
    return true;
}

static core_string_t
join(core_string_t parent, core_string_t child)
{
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_append_string(buffer, parent);
    core_buffer_puts(buffer, "/");
    core_buffer_append_string(buffer, child);
    return core_buffer_take_string(buffer);
}

static bool
write_file(core_string_t path, core_string_t contents)
{
    char *native = core_string_to_cstr(path);
    FILE *file = fopen(native, "wb");
    core_free(native);
    if (!file) return false;
    bool ok = fwrite(contents.data, 1, contents.u8_bytes, file)
        == contents.u8_bytes;
    return fclose(file) == 0 && ok;
}

static bool
make_directory(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    bool ok = mkdir(native, 0700) == 0;
    core_free(native);
    return ok;
}

static bool
remove_file(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    bool ok = unlink(native) == 0;
    core_free(native);
    return ok;
}

static bool
remove_directory(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    bool ok = rmdir(native) == 0;
    core_free(native);
    return ok;
}

static bool
path_exists(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    bool ok = access(native, F_OK) == 0;
    core_free(native);
    return ok;
}

static void
remove_directory_files(core_string_t directory)
{
    char *native = core_string_to_cstr(directory);
    DIR *files = opendir(native);
    core_free(native);
    if (!files) return;
    struct dirent *item;
    while ((item = readdir(files))) {
        core_string_t name = core_string_from_cstr(item->d_name);
        if (!core_string_eq(name, S(".")) && !core_string_eq(name, S(".."))) {
            core_string_t path = join(directory, name);
            remove_file(path);
            core_free(path.data);
        }
        core_free(name.data);
    }
    closedir(files);
}

static void
test_local_persistence(void)
{
    char directory[] = "/tmp/lantern-persist-XXXXXX";
    char *created = mkdtemp(directory);
    check("persistent test root exists", created != nullptr);
    if (!created) return;
    char cache_directory[] = "/tmp/lantern-persist-cache-XXXXXX";
    char *cache_created = mkdtemp(cache_directory);
    check("persistent cache root exists", cache_created != nullptr);
    if (!cache_created) return;
    char *cache_resolved = realpath(cache_created, nullptr);
    core_string_t cache_root = core_string_from_cstr(cache_resolved);
    free(cache_resolved);
    core_string_t root = core_string_from_cstr(created);
    core_string_t package = join(root, S("demo"));
    core_string_t source = join(package, S("facade.n"));
    core_string_t nested = join(package, S("inner"));
    core_string_t added = join(nested, S("added.n"));
    check("persistent package directory exists", make_directory(package));
    char *package_resolved = realpath(created, nullptr);
    core_string_t resolved_root = core_string_from_cstr(package_resolved);
    free(package_resolved);
    core_string_t resolved_package = join(resolved_root, S("demo"));
    core_string_t metadata = join(cache_root,
        core_string_slice(resolved_package, 1, resolved_package.u8_bytes - 1));
    core_string_t cache = join(metadata, S("inventory-v1"));
    core_string_t in_tree = join(package, S(".n00b-cache"));
    check("persistent source writes",
          write_file(source, S("package demo {}")));
    core_snapshot_inventory_t *first = core_snapshot_inventory_new(
        &root, 1, accept_source, nullptr);
    check("out-of-tree cache enables", core_snapshot_inventory_enable_cache(
        first, cache_root, 1));
    check("cold package refresh succeeds", core_snapshot_inventory_refresh(first));
    check("cold refresh writes metadata under the cache root", path_exists(cache));
    check("cold refresh writes nothing inside the package", !path_exists(in_tree));
    check("cold refresh reads its source", core_snapshot_inventory_stats(first).source_reads == 1);
    check("derived package names persist beside the package",
          core_snapshot_inventory_product_write(first, 0,
              S("demo/facade.n"), S("package-syntax"), 7, "demo", 4));
    core_snapshot_inventory_free(first);

    core_snapshot_inventory_t *second = core_snapshot_inventory_new(
        &root, 1, accept_source, nullptr);
    core_snapshot_inventory_enable_cache(second, cache_root, 1);
    check("warm package refresh succeeds", core_snapshot_inventory_refresh(second));
    core_snapshot_inventory_stats_t warm = core_snapshot_inventory_stats(second);
    check("new process reuses package metadata without reading source",
          warm.package_cache_hits == 1 && warm.source_reads == 0);
    const core_source_snapshot_t *snapshot = core_snapshot_inventory_find(
        second, 0, S("demo/facade.n"));
    check("warm snapshot retains source contents",
          snapshot && core_string_eq(core_source_snapshot_contents(snapshot),
                                     S("package demo {}")));
    core_buffer_t product = {0};
    check("derived product survives a new inventory instance",
          core_snapshot_inventory_product_read(second, 0,
              S("demo/facade.n"), S("package-syntax"), 7, &product)
          && product.byte_len == 4 && !memcmp(product.data, "demo", 4));
    core_buffer_release(&product);
    check("a different grammar profile misses the product",
          !core_snapshot_inventory_product_read(second, 0,
              S("demo/facade.n"), S("package-syntax"), 8, &product));

    check("source edit writes", write_file(source, S("package changed {}")));
    check("source edit refresh succeeds", core_snapshot_inventory_refresh(second));
    snapshot = core_snapshot_inventory_find(second, 0, S("demo/facade.n"));
    check("edited package is rebuilt", snapshot
          && core_string_eq(core_source_snapshot_contents(snapshot),
                            S("package changed {}"))
          && core_snapshot_inventory_stats(second).source_reads > 0);
    check("source edit invalidates its derived product",
          !core_snapshot_inventory_product_read(second, 0,
              S("demo/facade.n"), S("package-syntax"), 7, &product));

    check("new nested directory exists", make_directory(nested));
    check("new nested source writes", write_file(added, S("package added {}")));
    check("addition refresh succeeds", core_snapshot_inventory_refresh(second));
    check("new source appears", core_snapshot_inventory_find(second, 0,
        S("demo/inner/added.n")) != nullptr);
    check("nested source removes", remove_file(added));
    check("nested directory removes", remove_directory(nested));
    check("removal refresh succeeds", core_snapshot_inventory_refresh(second));
    check("removed source disappears", core_snapshot_inventory_find(second, 0,
        S("demo/inner/added.n")) == nullptr);
    core_snapshot_inventory_free(second);

    check("corrupt metadata writes", write_file(cache, S("broken")));
    core_snapshot_inventory_t *third = core_snapshot_inventory_new(
        &root, 1, accept_source, nullptr);
    core_snapshot_inventory_enable_cache(third, cache_root, 1);
    check("corrupt metadata falls back to source scan",
          core_snapshot_inventory_refresh(third)
          && core_snapshot_inventory_find(third, 0,
              S("demo/facade.n")) != nullptr
          && core_snapshot_inventory_stats(third).source_reads > 0);
    core_snapshot_inventory_free(third);
    remove_file(source);
    remove_file(cache);
    core_string_t derived = join(metadata, S("derived"));
    remove_directory_files(derived);
    remove_directory(derived);
    remove_directory(metadata);
    remove_directory(package);
    remove_directory(root);
    core_string_t cache_parent = core_string_dup(metadata);
    while (cache_parent.u8_bytes > cache_root.u8_bytes) {
        int64_t slash = core_string_rfind_byte(cache_parent, '/');
        if (slash <= 0) break;
        cache_parent.u8_bytes = (size_t)slash;
        remove_directory(cache_parent);
    }
    core_free(cache_parent.data);
    core_free(in_tree.data);
    core_free(resolved_package.data);
    core_free(resolved_root.data);
    core_free(cache_root.data);
    core_free(derived.data);
    core_free(added.data);
    core_free(nested.data);
    core_free(cache.data);
    core_free(metadata.data);
    core_free(source.data);
    core_free(package.data);
    core_free(root.data);
}

int
main(void)
{
    char directory[] = "/tmp/lantern-inventory-XXXXXX";
    char *created = mkdtemp(directory);
    check("temporary root exists", created != nullptr);
    if (!created) return report("source snapshot inventory");
    core_string_t root = core_string_from_cstr(created);
    core_string_t subdir = join(root, S("pkg"));
    core_string_t source_path = join(subdir, S("manifest.n"));
    core_string_t ignored_path = join(root, S("notes.txt"));
    check("source directory exists", make_directory(subdir));
    check("initial source writes",
          write_file(source_path, S("package one {}")));
    check("ignored file writes", write_file(ignored_path, S("ignored")));

    core_snapshot_inventory_t *inventory = core_snapshot_inventory_new(
        &root, 1, accept_source, nullptr);
    check("inventory initializes", inventory != nullptr);
    check("first refresh succeeds", core_snapshot_inventory_refresh(inventory));
    observed_t first = {0};
    check("inventory visits snapshots", core_snapshot_inventory_visit(
        inventory, observe, &first));
    check("filter retains only the source", first.count == 1 && first.source);
    check("lookup returns the visited snapshot",
          core_snapshot_inventory_find(inventory, 0,
              S("pkg/manifest.n")) == first.source);
    check("lookup does not return an ignored file",
          core_snapshot_inventory_find(inventory, 0,
              S("notes.txt")) == nullptr);
    uint64_t generation = core_snapshot_inventory_generation(inventory);
    uint64_t revision = core_source_snapshot_revision(first.source);
    core_source_snapshot_t *held = core_source_snapshot_retain(first.source);

    check("unchanged refresh succeeds", core_snapshot_inventory_refresh(inventory));
    observed_t same = {0};
    core_snapshot_inventory_visit(inventory, observe, &same);
    check("unchanged refresh preserves identity and generation",
          same.source == first.source
          && core_snapshot_inventory_generation(inventory) == generation);

    check("edited source writes",
          write_file(source_path, S("package second {}")));
    check("edited refresh succeeds", core_snapshot_inventory_refresh(inventory));
    observed_t edited = {0};
    core_snapshot_inventory_visit(inventory, observe, &edited);
    check("edited source has a new revision",
          edited.source && core_source_snapshot_revision(edited.source) > revision
          && core_snapshot_inventory_generation(inventory) > generation);
    check("pinned old snapshot keeps its contents",
          core_source_snapshot_size(held) == S("package one {}").u8_bytes
          && core_string_eq(core_source_snapshot_contents(held),
                            S("package one {}")));

    check("source removes", remove_file(source_path));
    check("removed refresh succeeds", core_snapshot_inventory_refresh(inventory));
    check("removed source leaves inventory", core_snapshot_inventory_count(inventory) == 0);
    core_source_snapshot_release(held);
    core_snapshot_inventory_free(inventory);
    remove_file(ignored_path);
    remove_directory(subdir);
    remove_directory(root);
    core_free(ignored_path.data);
    core_free(source_path.data);
    core_free(subdir.data);
    core_free(root.data);
    test_local_persistence();
    return report("source snapshot inventory");
}
