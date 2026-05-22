/** @file src/attest/n00b_attest_xdg.c — n00b-attest XDG path resolver.
 *
 *  Implementation of the public surface declared in
 *  include/attest/n00b_attest_xdg.h. Generalizes the byte-identical
 *  clone pattern that previously lived as two static helpers:
 *
 *    - src/attest/oci/auth.c:`resolve_registries_json_path`
 *      (registries.json lookup; D-052 OQ-1).
 *    - src/chalk/resign_identity.c:`xdg_signing_identity_path`
 *      (signing-identities lookup; WP-005 P4).
 *
 *  Both consumers now switch to this shared helper; byte-identical
 *  paths are preserved (the static clones differed only in the
 *  embedded suffix string, which now flows through as the caller's
 *  @p suffix parameter).
 *
 *  getenv() per D-052 (project-local libc exception for env-var
 *  config discovery). Future libn00b n00b_getenv lift = DF-010.
 */

#include "n00b.h"
#include "attest/n00b_attest_xdg.h"

#include "core/string.h"
#include "core/alloc.h"

#include <stdlib.h>   // getenv (D-052 libc exception for env-var config discovery)
#include <string.h>

n00b_string_t *
n00b_attest_xdg_path(n00b_string_t *suffix) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (suffix == nullptr || suffix->u8_bytes == 0) {
        return nullptr;
    }

    // getenv() per D-052 (project-local libc exception for env-var
    // config discovery). Future libn00b n00b_getenv lift = DF-010.
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *base;
    const char *base_suffix;
    size_t      base_len;
    size_t      base_suffix_len;

    if (xdg != nullptr && xdg[0] != '\0') {
        base            = xdg;
        base_len        = strlen(xdg);
        base_suffix     = "/n00b-attest/";
        base_suffix_len = strlen(base_suffix);
    } else {
        const char *home = getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            return nullptr;
        }
        base            = home;
        base_len        = strlen(home);
        base_suffix     = "/.config/n00b-attest/";
        base_suffix_len = strlen(base_suffix);
    }

    size_t suffix_len = (size_t)suffix->u8_bytes;
    size_t total_len  = base_len + base_suffix_len + suffix_len;
    char  *buf        = n00b_alloc_array_with_opts(
        char,
        total_len + 1,
        &(n00b_alloc_opts_t){.allocator = allocator});
    size_t off = 0;
    memcpy(buf + off, base, base_len);
    off += base_len;
    memcpy(buf + off, base_suffix, base_suffix_len);
    off += base_suffix_len;
    memcpy(buf + off, suffix->data, suffix_len);
    off += suffix_len;
    buf[total_len] = '\0';

    return n00b_string_from_raw(buf,
                                (int64_t)total_len,
                                .allocator = allocator);
}
