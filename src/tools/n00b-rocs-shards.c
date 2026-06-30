#include "n00b.h"
#include "rocs/map.h"
#include "rocs/store.h"
#include "rocs/wax.h"
#include "vfs/backend_local.h"
#include "vfs/vfs.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    ROCS_SHARDS_CMD_NONE,
    ROCS_SHARDS_CMD_LIST,
    ROCS_SHARDS_CMD_VERIFY,
    ROCS_SHARDS_CMD_SCAN,
    ROCS_SHARDS_CMD_DROP,
} rocs_shards_cmd_t;

typedef struct {
    rocs_shards_cmd_t  cmd;
    n00b_string_t     *store_root;
    uint64_t           shard_id;
    bool               has_shard_id;
    bool               yes;
} rocs_shards_args_t;

static void
usage(void)
{
    fprintf(stderr,
            "usage: n00b-rocs-shards --store-root PATH list\n"
            "       n00b-rocs-shards --store-root PATH verify --shard-id N\n"
            "       n00b-rocs-shards --store-root PATH scan\n"
            "       n00b-rocs-shards --store-root PATH drop --shard-id N --yes\n");
}

static void
print_string(n00b_string_t *s)
{
    if (s == NULL || s->data == NULL) {
        return;
    }
    printf("%.*s", (int)s->u8_bytes, s->data);
}

static bool
parse_u64_arg(const char *s, uint64_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static bool
parse_args(int argc, char **argv, rocs_shards_args_t *args)
{
    *args = (rocs_shards_args_t){0};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--store-root") == 0) {
            if (++i >= argc) {
                return false;
            }
            args->store_root = n00b_string_from_cstr(argv[i]);
            continue;
        }
        if (strcmp(argv[i], "--shard-id") == 0) {
            if (++i >= argc || !parse_u64_arg(argv[i], &args->shard_id)) {
                return false;
            }
            args->has_shard_id = true;
            continue;
        }
        if (strcmp(argv[i], "--yes") == 0) {
            args->yes = true;
            continue;
        }
        if (strcmp(argv[i], "list") == 0) {
            args->cmd = ROCS_SHARDS_CMD_LIST;
            continue;
        }
        if (strcmp(argv[i], "verify") == 0) {
            args->cmd = ROCS_SHARDS_CMD_VERIFY;
            continue;
        }
        if (strcmp(argv[i], "scan") == 0) {
            args->cmd = ROCS_SHARDS_CMD_SCAN;
            continue;
        }
        if (strcmp(argv[i], "drop") == 0) {
            args->cmd = ROCS_SHARDS_CMD_DROP;
            continue;
        }
        return false;
    }

    if (args->store_root == NULL || args->cmd == ROCS_SHARDS_CMD_NONE) {
        return false;
    }
    if ((args->cmd == ROCS_SHARDS_CMD_VERIFY || args->cmd == ROCS_SHARDS_CMD_DROP)
        && (!args->has_shard_id || args->shard_id == 0)) {
        return false;
    }
    if (args->cmd != ROCS_SHARDS_CMD_DROP && args->yes) {
        return false;
    }
    return true;
}

static int
print_store_err(const char *op, n00b_err_t err)
{
    fprintf(stderr,
            "n00b-rocs-shards: %s failed: ",
            op);
    print_string(n00b_store_err_str(err));
    fprintf(stderr, " (%" PRId64 ")\n", (int64_t)err);
    return 1;
}

static int
print_map_err(const char *op, n00b_err_t err)
{
    fprintf(stderr,
            "n00b-rocs-shards: %s failed: ",
            op);
    print_string(n00b_store_map_err_str(err));
    fprintf(stderr, " (%" PRId64 ")\n", (int64_t)err);
    return 1;
}

static n00b_result_t(n00b_store_t *)
open_wax_store(n00b_string_t *local_store_root)
{
    auto schema_r = n00b_rocs_wax_schema_new();
    if (n00b_result_is_err(schema_r)) {
        return n00b_result_err(n00b_store_t *, N00B_STORE_ERR_CONFIG);
    }
    auto partition_r = n00b_rocs_wax_partition_policy_new();
    auto seal_r      = n00b_rocs_wax_seal_policy_new();
    if (n00b_result_is_err(partition_r) || n00b_result_is_err(seal_r)) {
        return n00b_result_err(n00b_store_t *, N00B_STORE_ERR_POLICY);
    }

    auto vfs_r = n00b_vfs_new();
    if (n00b_result_is_err(vfs_r)) {
        return n00b_result_err(n00b_store_t *, N00B_STORE_ERR_VFS);
    }
    n00b_vfs_t *vfs = n00b_result_get(vfs_r);

    n00b_string_t *backend_root = local_store_root;
    if (local_store_root != NULL && local_store_root->u8_bytes > 6
        && memcmp(local_store_root->data + local_store_root->u8_bytes - 6,
                  "/store",
                  6) == 0) {
        backend_root = n00b_string_from_raw(local_store_root->data,
                                            (int64_t)local_store_root->u8_bytes - 6);
    }

    auto backend_r = n00b_vfs_backend_local_new(backend_root);
    if (n00b_result_is_err(backend_r)) {
        return n00b_result_err(n00b_store_t *, N00B_STORE_ERR_VFS);
    }
    auto mount_r = n00b_vfs_mount(vfs, r"/", n00b_result_get(backend_r), 0);
    if (n00b_result_is_err(mount_r)) {
        return n00b_result_err(n00b_store_t *, N00B_STORE_ERR_VFS);
    }

    static n00b_store_residency_policy_t policy;
    policy                     = n00b_store_residency_policy_get_default();
    policy.preferred_backing   = N00B_STORE_IMAGE_LOCAL_MMAP;
    policy.allow_direct_mmap   = true;
    policy.validate_on_open    = false;
    policy.max_resident_bytes  = 0;
    policy.max_resident_shards = 1;

    return n00b_store_open_vfs(vfs,
                               r"/store",
                               n00b_result_get(schema_r),
                               .partition_policy = n00b_result_get(partition_r),
                               .seal_policy      = n00b_result_get(seal_r),
                               .residency_policy = &policy,
                               .display_name     = r"n00b-rocs-shards");
}

static int
run_list(n00b_store_t *store)
{
    auto count_r = n00b_store_catalog_visible_entry_count(store);
    if (n00b_result_is_err(count_r)) {
        return print_store_err("catalog count", n00b_result_get_err(count_r));
    }

    uint64_t count = n00b_result_get(count_r);
    printf("sealed_shards=%" PRIu64 "\n", count);
    for (uint64_t i = 0; i < count; i++) {
        auto entry_r = n00b_store_catalog_visible_entry_at(store, i);
        if (n00b_result_is_err(entry_r)) {
            return print_store_err("catalog entry", n00b_result_get_err(entry_r));
        }
        n00b_option_t(n00b_store_catalog_entry_t *) opt = n00b_result_get(entry_r);
        if (!n00b_option_is_set(opt)) {
            continue;
        }
        n00b_store_catalog_entry_t *entry = n00b_option_get(opt);
        auto id_r    = n00b_store_catalog_entry_get_shard_id(entry);
        auto gen_r   = n00b_store_catalog_entry_get_generation(entry);
        auto rec_r   = n00b_store_catalog_entry_get_record_count(entry);
        auto bytes_r = n00b_store_catalog_entry_get_byte_len(entry);
        auto path_r  = n00b_store_catalog_entry_get_object_path(entry);
        if (n00b_result_is_err(id_r) || n00b_result_is_err(gen_r)
            || n00b_result_is_err(rec_r) || n00b_result_is_err(bytes_r)
            || n00b_result_is_err(path_r)) {
            return print_store_err("catalog entry fields", N00B_STORE_ERR_CORRUPT);
        }
        printf("shard=%" PRIu64 " generation=%" PRIu64
               " records=%" PRIu64 " bytes=%" PRIu64 " path=",
               n00b_result_get(id_r),
               n00b_result_get(gen_r),
               n00b_result_get(rec_r),
               n00b_result_get(bytes_r));
        print_string(n00b_result_get(path_r));
        printf("\n");
    }
    return 0;
}

static int
find_entry(n00b_store_t *store,
           uint64_t      shard_id,
           n00b_store_catalog_entry_t **out)
{
    auto find_r = n00b_store_catalog_find_shard(store, shard_id);
    if (n00b_result_is_err(find_r)) {
        return print_store_err("find shard", n00b_result_get_err(find_r));
    }
    n00b_option_t(n00b_store_catalog_entry_t *) opt = n00b_result_get(find_r);
    if (!n00b_option_is_set(opt)) {
        fprintf(stderr,
                "n00b-rocs-shards: shard %" PRIu64 " is not in catalog\n",
                shard_id);
        return 1;
    }
    *out = n00b_option_get(opt);
    return 0;
}

static int
scan_store_err(uint64_t shard_id, const char *op, n00b_err_t err)
{
    printf("shard=%" PRIu64 " verify=fail op=%s code=%" PRId64 " label=",
           shard_id,
           op,
           (int64_t)err);
    print_string(n00b_store_err_str(err));
    printf("\n");
    return 1;
}

static int
scan_map_err(uint64_t shard_id, const char *op, n00b_err_t err)
{
    printf("shard=%" PRIu64 " verify=fail op=%s code=%" PRId64 " label=",
           shard_id,
           op,
           (int64_t)err);
    print_string(n00b_store_map_err_str(err));
    printf("\n");
    return 1;
}

static int
verify_entry(n00b_store_t              *store,
             n00b_store_catalog_entry_t *entry,
             uint64_t                   shard_id,
             bool                       verbose)
{
    auto resident_r = n00b_store_resident_shard_acquire(store, entry);
    if (n00b_result_is_err(resident_r)) {
        n00b_err_t err = n00b_result_get_err(resident_r);
        if (verbose) {
            return print_store_err("resident acquire", err);
        }
        return scan_store_err(shard_id, "resident_acquire", err);
    }
    n00b_store_resident_shard_t *resident = n00b_result_get(resident_r);

    auto map_r = n00b_store_resident_shard_map(resident);
    if (n00b_result_is_err(map_r)) {
        (void)n00b_store_resident_shard_release(resident);
        n00b_err_t err = n00b_result_get_err(map_r);
        if (verbose) {
            return print_store_err("resident map", err);
        }
        return scan_store_err(shard_id, "resident_map", err);
    }

    auto root_r = n00b_store_map_root(n00b_result_get(map_r));
    if (n00b_result_is_err(root_r)) {
        (void)n00b_store_resident_shard_release(resident);
        n00b_err_t err = n00b_result_get_err(root_r);
        if (verbose) {
            return print_map_err("map root", err);
        }
        return scan_map_err(shard_id, "map_root", err);
    }
    n00b_store_map_shard_t *root = n00b_result_get(root_r);

    auto id_r      = n00b_store_map_shard_id(root);
    auto records_r = n00b_store_map_shard_records_len(root);
    auto list_r    = n00b_store_map_shard_records(root);
    if (n00b_result_is_err(id_r)) {
        (void)n00b_store_resident_shard_release(resident);
        n00b_err_t err = n00b_result_get_err(id_r);
        if (verbose) {
            return print_map_err("mapped shard id", err);
        }
        return scan_map_err(shard_id, "mapped_shard_id", err);
    }
    if (n00b_result_is_err(records_r)) {
        (void)n00b_store_resident_shard_release(resident);
        n00b_err_t err = n00b_result_get_err(records_r);
        if (verbose) {
            return print_map_err("mapped record count", err);
        }
        return scan_map_err(shard_id, "mapped_record_count", err);
    }
    if (n00b_result_is_err(list_r)) {
        (void)n00b_store_resident_shard_release(resident);
        n00b_err_t err = n00b_result_get_err(list_r);
        if (verbose) {
            return print_map_err("mapped records list", err);
        }
        return scan_map_err(shard_id, "mapped_records_list", err);
    }

    uint64_t records = n00b_result_get(records_r);
    if (records != 0) {
        auto slot_r = n00b_store_map_list_slot(n00b_result_get(list_r), 0);
        if (n00b_result_is_err(slot_r)) {
            (void)n00b_store_resident_shard_release(resident);
            n00b_err_t err = n00b_result_get_err(slot_r);
            if (verbose) {
                return print_map_err("mapped records slot[0]", err);
            }
            return scan_map_err(shard_id, "mapped_records_slot_0", err);
        }
    }

    auto release_r = n00b_store_resident_shard_release(resident);
    if (n00b_result_is_err(release_r)) {
        n00b_err_t err = n00b_result_get_err(release_r);
        if (verbose) {
            return print_store_err("resident release", err);
        }
        return scan_store_err(shard_id, "resident_release", err);
    }

    if (verbose) {
        printf("shard=%" PRIu64 " mapped_id=%" PRIu64
               " records=%" PRIu64 " verify=ok\n",
               shard_id,
               n00b_result_get(id_r),
               records);
    }
    return 0;
}

static int
run_verify(n00b_store_t *store, uint64_t shard_id)
{
    n00b_store_catalog_entry_t *entry = NULL;
    int rc = find_entry(store, shard_id, &entry);
    if (rc != 0) {
        return rc;
    }

    return verify_entry(store, entry, shard_id, true);
}

static int
run_scan(n00b_store_t *store)
{
    auto count_r = n00b_store_catalog_visible_entry_count(store);
    if (n00b_result_is_err(count_r)) {
        return print_store_err("catalog count", n00b_result_get_err(count_r));
    }

    uint64_t count = n00b_result_get(count_r);
    uint64_t bad   = 0;
    for (uint64_t i = 0; i < count; i++) {
        auto entry_r = n00b_store_catalog_visible_entry_at(store, i);
        if (n00b_result_is_err(entry_r)) {
            return print_store_err("catalog entry", n00b_result_get_err(entry_r));
        }
        n00b_option_t(n00b_store_catalog_entry_t *) opt = n00b_result_get(entry_r);
        if (!n00b_option_is_set(opt)) {
            continue;
        }
        n00b_store_catalog_entry_t *entry = n00b_option_get(opt);
        auto id_r = n00b_store_catalog_entry_get_shard_id(entry);
        if (n00b_result_is_err(id_r)) {
            return print_store_err("catalog entry shard id",
                                   n00b_result_get_err(id_r));
        }
        if (verify_entry(store, entry, n00b_result_get(id_r), false) != 0) {
            bad++;
        }
    }

    printf("sealed_shards=%" PRIu64 " bad_shards=%" PRIu64 "\n", count, bad);
    return bad == 0 ? 0 : 1;
}

static int
run_drop(n00b_store_t *store, uint64_t shard_id, bool yes)
{
    if (!yes) {
        fprintf(stderr,
                "n00b-rocs-shards: refusing to drop shard %" PRIu64
                " without --yes\n",
                shard_id);
        return 1;
    }

    n00b_store_catalog_entry_t *entry = NULL;
    int rc = find_entry(store, shard_id, &entry);
    if (rc != 0) {
        return rc;
    }
    (void)entry;

    auto drop_r = n00b_store_drop_sealed_shard(store,
                                               shard_id,
                                               .drop_reason = r"operator");
    if (n00b_result_is_err(drop_r)) {
        return print_store_err("drop shard", n00b_result_get_err(drop_r));
    }
    if (!n00b_result_get(drop_r)) {
        fprintf(stderr,
                "n00b-rocs-shards: shard %" PRIu64 " was not dropped\n",
                shard_id);
        return 1;
    }
    printf("shard=%" PRIu64 " dropped=1\n", shard_id);
    return 0;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    rocs_shards_args_t args = {0};
    if (!parse_args(argc, argv, &args)) {
        usage();
        return 2;
    }

    auto store_r = open_wax_store(args.store_root);
    if (n00b_result_is_err(store_r)) {
        return print_store_err("open store", n00b_result_get_err(store_r));
    }
    n00b_store_t *store = n00b_result_get(store_r);

    int rc = 0;
    switch (args.cmd) {
    case ROCS_SHARDS_CMD_LIST:
        rc = run_list(store);
        break;
    case ROCS_SHARDS_CMD_VERIFY:
        rc = run_verify(store, args.shard_id);
        break;
    case ROCS_SHARDS_CMD_SCAN:
        rc = run_scan(store);
        break;
    case ROCS_SHARDS_CMD_DROP:
        rc = run_drop(store, args.shard_id, args.yes);
        break;
    case ROCS_SHARDS_CMD_NONE:
        usage();
        rc = 2;
        break;
    }

    auto close_r = n00b_store_close(store);
    if (rc == 0 && n00b_result_is_err(close_r)) {
        return print_store_err("close store", n00b_result_get_err(close_r));
    }
    return rc;
}
