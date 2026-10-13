#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core.h"
#include "lib/snapshot_inventory.h"
#include "harness.h"

typedef struct {
    const core_source_snapshot_t *source;
    size_t count;
} observed_t;

static bool
accept_source(void *context, core_string_t relative)
{
    (void)context;
    return relative.u8_bytes >= 2
        && !memcmp(relative.data + relative.u8_bytes - 2, ".n", 2);
}

static bool
observe(void *context, size_t root, core_string_t relative,
        const core_source_snapshot_t *source)
{
    observed_t *result = context;
    result->count++;
    if (root == 0 && core_string_eq(relative, CORE_STRING_STATIC("pkg/manifest.n")))
        result->source = source;
    return true;
}

static bool
write_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    size_t size = strlen(contents);
    bool ok = fwrite(contents, 1, size, file) == size;
    return fclose(file) == 0 && ok;
}

static void
test_local_persistence(void)
{
    char directory[] = "/tmp/lantern-persist-XXXXXX";
    char *root = mkdtemp(directory);
    check("persistent test root exists", root != nullptr);
    if (!root) return;
    char package[256], source[256], metadata[256], cache[256];
    char nested[256], added[256];
    snprintf(package, sizeof package, "%s/demo", root);
    snprintf(source, sizeof source, "%s/facade.n", package);
    snprintf(metadata, sizeof metadata, "%s/.n00b-cache", package);
    snprintf(cache, sizeof cache, "%s/inventory-v1", metadata);
    snprintf(nested, sizeof nested, "%s/inner", package);
    snprintf(added, sizeof added, "%s/added.n", nested);
    check("persistent package directory exists", mkdir(package, 0700) == 0);
    check("persistent source writes", write_file(source, "package demo {}"));
    core_string_t root_name = core_string_from_cstr(root);
    core_snapshot_inventory_t *first = core_snapshot_inventory_new(
        &root_name, 1, accept_source, nullptr);
    check("package-local cache enables", core_snapshot_inventory_enable_local_cache(
        first, CORE_STRING_STATIC(".n00b-cache"), 1));
    check("cold package refresh succeeds", core_snapshot_inventory_refresh(first));
    check("cold refresh writes metadata inside the package", access(cache, F_OK) == 0);
    check("cold refresh reads its source", core_snapshot_inventory_stats(first).source_reads == 1);
    check("derived package names persist beside the package",
          core_snapshot_inventory_product_write(first, 0,
              CORE_STRING_STATIC("demo/facade.n"),
              CORE_STRING_STATIC("package-syntax"), 7, "demo", 4));
    core_snapshot_inventory_free(first);

    core_snapshot_inventory_t *second = core_snapshot_inventory_new(
        &root_name, 1, accept_source, nullptr);
    core_snapshot_inventory_enable_local_cache(second,
        CORE_STRING_STATIC(".n00b-cache"), 1);
    check("warm package refresh succeeds", core_snapshot_inventory_refresh(second));
    core_snapshot_inventory_stats_t warm = core_snapshot_inventory_stats(second);
    check("new process reuses package metadata without reading source",
          warm.package_cache_hits == 1 && warm.source_reads == 0);
    const core_source_snapshot_t *snapshot = core_snapshot_inventory_find(
        second, 0, CORE_STRING_STATIC("demo/facade.n"));
    check("warm snapshot retains source contents",
          snapshot && !strcmp(core_source_snapshot_contents(snapshot),
                              "package demo {}"));
    core_buffer_t product = {0};
    check("derived product survives a new inventory instance",
          core_snapshot_inventory_product_read(second, 0,
              CORE_STRING_STATIC("demo/facade.n"),
              CORE_STRING_STATIC("package-syntax"), 7, &product)
          && product.byte_len == 4 && !memcmp(product.data, "demo", 4));
    core_buffer_release(&product);
    check("a different grammar profile misses the product",
          !core_snapshot_inventory_product_read(second, 0,
              CORE_STRING_STATIC("demo/facade.n"),
              CORE_STRING_STATIC("package-syntax"), 8, &product));

    check("source edit writes", write_file(source, "package changed {}"));
    check("source edit refresh succeeds", core_snapshot_inventory_refresh(second));
    snapshot = core_snapshot_inventory_find(second, 0,
        CORE_STRING_STATIC("demo/facade.n"));
    check("edited package is rebuilt", snapshot
          && !strcmp(core_source_snapshot_contents(snapshot),
                     "package changed {}")
          && core_snapshot_inventory_stats(second).source_reads > 0);
    check("source edit invalidates its derived product",
          !core_snapshot_inventory_product_read(second, 0,
              CORE_STRING_STATIC("demo/facade.n"),
              CORE_STRING_STATIC("package-syntax"), 7, &product));

    check("new nested directory exists", mkdir(nested, 0700) == 0);
    check("new nested source writes", write_file(added, "package added {}"));
    check("addition refresh succeeds", core_snapshot_inventory_refresh(second));
    check("new source appears", core_snapshot_inventory_find(second, 0,
        CORE_STRING_STATIC("demo/inner/added.n")) != nullptr);
    check("nested source removes", unlink(added) == 0);
    check("nested directory removes", rmdir(nested) == 0);
    check("removal refresh succeeds", core_snapshot_inventory_refresh(second));
    check("removed source disappears", core_snapshot_inventory_find(second, 0,
        CORE_STRING_STATIC("demo/inner/added.n")) == nullptr);
    core_snapshot_inventory_free(second);

    check("corrupt metadata writes", write_file(cache, "broken"));
    core_snapshot_inventory_t *third = core_snapshot_inventory_new(
        &root_name, 1, accept_source, nullptr);
    core_snapshot_inventory_enable_local_cache(third,
        CORE_STRING_STATIC(".n00b-cache"), 1);
    check("corrupt metadata falls back to source scan",
          core_snapshot_inventory_refresh(third)
          && core_snapshot_inventory_find(third, 0,
              CORE_STRING_STATIC("demo/facade.n")) != nullptr
          && core_snapshot_inventory_stats(third).source_reads > 0);
    core_snapshot_inventory_free(third);
    free(root_name.data);
    unlink(source);
    unlink(cache);
    char derived[256];
    snprintf(derived, sizeof derived, "%s/derived", metadata);
    DIR *files = opendir(derived);
    if (files) {
        struct dirent *item;
        while ((item = readdir(files))) {
            if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
                continue;
            char path[512];
            snprintf(path, sizeof path, "%s/%s", derived, item->d_name);
            unlink(path);
        }
        closedir(files);
        rmdir(derived);
    }
    rmdir(metadata);
    rmdir(package);
    rmdir(root);
}

int
main(void)
{
    char directory[] = "/tmp/lantern-inventory-XXXXXX";
    char *root = mkdtemp(directory);
    check("temporary root exists", root != nullptr);
    if (!root) return report("source snapshot inventory");
    char subdir[256], source_path[256], ignored_path[256];
    snprintf(subdir, sizeof(subdir), "%s/pkg", root);
    snprintf(source_path, sizeof(source_path), "%s/manifest.n", subdir);
    snprintf(ignored_path, sizeof(ignored_path), "%s/notes.txt", root);
    check("source directory exists", mkdir(subdir, 0700) == 0);
    check("initial source writes", write_file(source_path, "package one {}"));
    check("ignored file writes", write_file(ignored_path, "ignored"));

    core_string_t root_name = core_string_from_cstr(root);
    core_snapshot_inventory_t *inventory = core_snapshot_inventory_new(
        &root_name, 1, accept_source, nullptr);
    check("inventory initializes", inventory != nullptr);
    check("first refresh succeeds", core_snapshot_inventory_refresh(inventory));
    observed_t first = {0};
    check("inventory visits snapshots", core_snapshot_inventory_visit(
        inventory, observe, &first));
    check("filter retains only the source", first.count == 1 && first.source);
    check("lookup returns the visited snapshot",
          core_snapshot_inventory_find(inventory, 0,
              CORE_STRING_STATIC("pkg/manifest.n")) == first.source);
    check("lookup does not return an ignored file",
          core_snapshot_inventory_find(inventory, 0,
              CORE_STRING_STATIC("notes.txt")) == nullptr);
    uint64_t generation = core_snapshot_inventory_generation(inventory);
    uint64_t revision = core_source_snapshot_revision(first.source);
    core_source_snapshot_t *held = core_source_snapshot_retain(first.source);

    check("unchanged refresh succeeds", core_snapshot_inventory_refresh(inventory));
    observed_t same = {0};
    core_snapshot_inventory_visit(inventory, observe, &same);
    check("unchanged refresh preserves identity and generation",
          same.source == first.source
          && core_snapshot_inventory_generation(inventory) == generation);

    check("edited source writes", write_file(source_path, "package second {}"));
    check("edited refresh succeeds", core_snapshot_inventory_refresh(inventory));
    observed_t edited = {0};
    core_snapshot_inventory_visit(inventory, observe, &edited);
    check("edited source has a new revision",
          edited.source && core_source_snapshot_revision(edited.source) > revision
          && core_snapshot_inventory_generation(inventory) > generation);
    check("pinned old snapshot keeps its contents",
          core_source_snapshot_size(held) == strlen("package one {}")
          && !memcmp(core_source_snapshot_contents(held), "package one {}",
                     strlen("package one {}")));

    check("source removes", unlink(source_path) == 0);
    check("removed refresh succeeds", core_snapshot_inventory_refresh(inventory));
    check("removed source leaves inventory", core_snapshot_inventory_count(inventory) == 0);
    core_source_snapshot_release(held);
    core_snapshot_inventory_free(inventory);
    free(root_name.data);
    unlink(ignored_path);
    rmdir(subdir);
    rmdir(root);
    test_local_persistence();
    return report("source snapshot inventory");
}
