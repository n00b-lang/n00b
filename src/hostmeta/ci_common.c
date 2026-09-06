/** @file src/hostmeta/ci_common.c — shared surface for the CI collectors.
 *
 *  Nine CI systems, one `BUILD_*` vocabulary. These helpers hold the
 *  phase scoping and the `BUILD_UNIQUE_ID` file protocol so each
 *  per-vendor file is just its variable-to-key mapping.
 */

#define N00B_USE_INTERNAL_API

#include "internal/hostmeta/hostmeta_internal.h"

#include "core/buffer.h"
#include "core/file.h"
#include "core/platform.h"
#include "core/random.h"
#include "core/time.h"
#include "util/path.h"

void
n00b_hostmeta_ci_put(n00b_hostmeta_ctx_t *ctx,
                     const char          *base,
                     n00b_string_t       *value)
{
    n00b_hostmeta_put_string(ctx, n00b_hostmeta_scoped_key(ctx, base), value);
}

void
n00b_hostmeta_ci_put_contact(n00b_hostmeta_ctx_t *ctx, n00b_string_t *value)
{
    n00b_hostmeta_put_string_list(ctx,
                                  n00b_hostmeta_scoped_key(ctx,
                                                           "BUILD_CONTACT"),
                                  value);
}

n00b_string_t *
n00b_hostmeta_random_hex64(void)
{
    static const char hex[] = "0123456789abcdef";

    uint64_t v = n00b_rand64();
    char     out[16];
    for (int i = 15; i >= 0; i--) {
        out[i] = hex[v & 0xf];
        v >>= 4;
    }
    return n00b_string_from_raw(out, 16);
}

n00b_string_t *
n00b_hostmeta_get_or_write_exclusive(n00b_string_t *path, n00b_string_t *value)
{
    if (path == nullptr || path->u8_bytes == 0 || value == nullptr) {
        return nullptr;
    }

    n00b_string_t *canonical = n00b_path_canonical(path);
    if (canonical == nullptr) {
        return nullptr;
    }

    auto fr = n00b_file_open_exclusive(canonical);
    if (n00b_result_is_ok(fr)) {
        n00b_file_t *f = n00b_result_get(fr);
        auto         w = n00b_file_write(f, value->data, value->u8_bytes);
        n00b_file_close(f);
        if (n00b_result_is_err(w)) {
            return nullptr;
        }
        return value;
    }

    // Lost the create race (or the path already held a value from an
    // earlier step in the same job): the winner's value is the answer.
    for (int i = 0; i <= 64; i++) {
        n00b_string_t *read = n00b_hostmeta_read_file(canonical);
        if (read != nullptr && read->u8_bytes == value->u8_bytes) {
            return read;
        }
        base_nanosleep_ns(1ULL * N00B_NS_PER_MS);
    }

    return nullptr;
}
