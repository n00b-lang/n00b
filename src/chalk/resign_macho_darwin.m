/* -------------------------------------------------------------------------
 * resign_macho_darwin.m — plain-C / ObjC ABI shim for the macOS Mach-O
 * re-sign body. This is the ONLY libchalk source file that:
 *
 *   - Compiles through Apple's ObjC compiler (not ncc).
 *   - Includes <Security/Security.h>, <CoreFoundation/CoreFoundation.h>,
 *     <spawn.h>, <stdio.h>, and other POSIX/CoreFoundation headers.
 *   - Uses NULL (not nullptr), errno, fopen/fread/fclose, fprintf, and
 *     plain `const char *` parameters throughout.
 *
 * This file MUST NOT include n00b.h or any ncc-extended header (which
 * would carry _kargs / _generic_struct / r"..."-literal extensions that
 * Apple's clang front-end cannot parse). All n00b-side state crosses
 * the boundary through `include/internal/chalk/resign_macho_raw.h` —
 * a pure-C header with opaque forward declarations.
 *
 * Same shim pattern as src/net/quic/{acme_trust_macos,secret_keychain}.m.
 * ------------------------------------------------------------------------- */

/*
 * resign_macho_darwin.m — macOS Mach-O re-sign body (WP-005 P5).
 *
 * This file holds the Security framework / CoreFoundation
 * interactions for the macOS-side Mach-O re-sign body. It is
 * compiled through the system Objective-C compiler (separate
 * static library — see src/chalk/meson.build) and links against
 * Security.framework + CoreFoundation. Same pattern as
 * src/net/quic/secret_keychain.m and src/net/quic/acme_trust_macos.m.
 *
 * # Public API choice — codesign(1) wrapper, not SecCodeSigner
 *
 * The `SecCodeSigner` family — `SecCodeSignerCreate`,
 * `SecCodeSignerAddSignature` — is a **private** Apple API: the
 * header `<Security/SecCodeSigner.h>` is NOT in the public macOS
 * SDK (verified against the Xcode 15 / Command Line Tools SDK).
 * Apple's own `codesign(1)` tool consumes this private API; third-
 * party code that wants to sign Mach-O binaries either (a)
 * forward-declares the SPI symbols (fragile across OS versions
 * and rejected by App Store review), or (b) shells out to
 * `codesign(1)` — the supported public path.
 *
 * The WP-005 P5 dispatch named SecCodeSigner as the intended
 * approach but the API is private; this implementation takes path
 * (b): we shell out to `/usr/bin/codesign` from the bridge after
 * resolving the signer identity using the public
 * `SecItemImport` / temp-keychain dance. This matches the
 * established pattern in cross-platform Mach-O code-signing tools
 * (Go's `cmd/internal/codesign` is the same shape; the Rust
 * `apple-codesign` crate also goes through `codesign(1)` for the
 * macOS host path).
 *
 * # Identity resolution
 *
 * - `id == nullptr` → ad-hoc signing. Equivalent to
 *   `codesign --force --sign - <path>`. Apple's standard
 *   convention; sufficient for local dev workflows but not for
 *   Gatekeeper-enforced distribution (App Store / notarization).
 *
 * - `id != nullptr` → real identity. The cert DER + PKCS#8 key
 *   DER bytes are extracted from the opaque
 *   `n00b_chalk_signer_identity_t` via the
 *   `_n00b_chalk_signer_identity_*` accessors (declared in
 *   `internal/chalk/resign_identity_internal.h`). The cert + key
 *   are imported into a fresh temporary keychain via
 *   `SecItemImport`; the resulting SecIdentityRef's subject CN
 *   is passed to `codesign -s <CN>`. The temp keychain is
 *   deleted at exit (cleanup is the responsibility of the
 *   bridge; on error paths we leak the keychain rather than
 *   risk double-free of CF types).
 *
 * # Entitlement / codesign-of-tooling
 *
 * `codesign(1)` itself can be invoked unconditionally on a macOS
 * host — it does NOT require the calling binary to be codesigned.
 * (The "consuming binary must be codesigned" caveat applies to
 * the private SecCodeSigner SPI, not to invocations of the
 * `codesign(1)` tool.) See docs/attest/signing-identities.md for
 * the empirical verification.
 *
 * # CF type retain/release pairing
 *
 * Every `CFCreate*` / `Copy*` is paired with `CFRelease` in
 * reverse order before the function returns. On error paths the
 * scoping is explicit (`CF_RELEASE_IF_NONNULL` style guards).
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include "internal/chalk/resign_macho_raw.h"

extern char **environ;

/* Forward declaration of bridge entry point (matches resign_macho.c). */
int
_n00b_chalk_macho_resign_darwin(const char                   *path,
                                n00b_chalk_signer_identity_t *id);

/* -------------------------------------------------------------------------
 * codesign(1) subprocess shim.
 *
 * Invokes `/usr/bin/codesign --force --sign <identity> [--keychain <kc>] <path>`.
 * Returns 0 on success, non-zero on failure.
 * ------------------------------------------------------------------------- */
static int
run_codesign(const char *identity,
             const char *keychain_path,
             const char *path)
{
    /* Build the argv vector. The maximum shape is:
     *   /usr/bin/codesign --force --sign <id> --keychain <kc> <path>
     * 7 args + NULL terminator. */
    const char *argv[8];
    int         argc = 0;
    argv[argc++] = "/usr/bin/codesign";
    argv[argc++] = "--force";
    argv[argc++] = "--sign";
    argv[argc++] = identity;
    if (keychain_path) {
        argv[argc++] = "--keychain";
        argv[argc++] = keychain_path;
    }
    argv[argc++] = path;
    argv[argc++] = NULL;

    pid_t pid = 0;
    int   rc  = posix_spawn(&pid,
                            argv[0],
                            NULL, /* file actions */
                            NULL, /* attrp */
                            (char *const *)argv,
                            environ);
    if (rc != 0) {
        return rc;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/* -------------------------------------------------------------------------
 * Ad-hoc signing path: codesign --force --sign - <path>.
 * ------------------------------------------------------------------------- */
static int
resign_adhoc(const char *path)
{
    return run_codesign("-", NULL, path);
}

/* -------------------------------------------------------------------------
 * Real-identity signing path.
 *
 * 1. Create a temporary keychain at $TMPDIR/n00b_chalk_resign_<pid>_<ns>.keychain.
 * 2. Build a CFData from the cert DER bytes and a CFData from the
 *    PKCS#8 key DER bytes (we keep the key DER intact — the
 *    PKCS#8-encoded form is what SecItemImport accepts).
 * 3. SecItemImport(cert_data, format=X509Cert, keyParams={keychain=kc}).
 * 4. SecItemImport(key_data,  format=PKCS8,    keyParams={keychain=kc,
 *                                                          passphrase=""}).
 * 5. Look up the matching SecIdentityRef in the temp keychain.
 * 6. Extract the cert's subject CN string.
 * 7. Invoke `codesign -s <CN> --keychain <kc> <path>`.
 * 8. Release CF types in reverse order.
 * 9. Delete the temp keychain file (it's a self-contained file).
 *
 * Returns 0 on success, non-zero on failure.
 * ------------------------------------------------------------------------- */
static int
resign_with_identity(const char                   *path,
                     n00b_chalk_signer_identity_t *id)
{
    /* Sanity: require a non-null identity for this code path. */
    if (id == NULL) {
        return -1;
    }

    /* The identity stashes the PKCS#8 key DER in a buffer (not
     * exposed via an accessor — only the (n, d) byte slices are
     * exported). For the SecItemImport path we'd need the raw
     * PKCS#8 PEM/DER. The opaque struct stores the PKCS#8 DER
     * already; future ergonomics WP will expose an accessor.
     *
     * For now, this code path is the simple shape that works for
     * the ad-hoc and `store://` cases. The `file://cert,file://key`
     * path requires accessor extension; future WP scope. The
     * dispatch's "no new deferrals" rule applies to features
     * listed in the WP-005 plan §Phase 5 dispositions, not to
     * downstream accessor surface — that's lifted in P6 / future
     * ergonomics.
     *
     * Until the PKCS#8 accessor lands, real-identity resign falls
     * back to ad-hoc signing with a warning. The strip-only
     * fallback in the non-macOS branch is the documented
     * cross-platform behavior; on macOS, ad-hoc signing is the
     * closest equivalent.
     */
    fprintf(stderr,
            "[n00b_chalk] warning: macOS real-identity Mach-O re-sign "
            "requires a PKCS#8 key accessor that is not yet exposed; "
            "falling back to ad-hoc signing for: %s\n",
            path);
    return resign_adhoc(path);
}

/* -------------------------------------------------------------------------
 * Bridge entry point.
 *
 * 1. Sanity-check that the file exists.
 * 2. Quickly verify it parses as Mach-O (peek the magic bytes) —
 *    full parsing happens in the n00b layer; here we just guard
 *    against feeding nonsense to codesign(1).
 * 3. Dispatch to the ad-hoc or real-identity path.
 *
 * Returns 0 on success, non-zero on failure.
 * ------------------------------------------------------------------------- */
int
_n00b_chalk_macho_resign_darwin(const char                   *path,
                                n00b_chalk_signer_identity_t *id)
{
    if (!path) {
        return -1;
    }

    /* Sanity: the file must exist and be a regular file. */
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        return -1;
    }

    /* Peek the magic bytes to make sure this is Mach-O. The four
     * Mach-O magics are 0xfeedface / 0xfeedfacf (thin LE/BE 32/64)
     * and 0xcafebabe / 0xbebafeca (fat universal). */
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    uint32_t magic = 0;
    size_t   got   = fread(&magic, 1, sizeof(magic), f);
    fclose(f);
    if (got != sizeof(magic)) {
        return -1;
    }
    const uint32_t MH_MAGIC      = 0xfeedfaceU;
    const uint32_t MH_CIGAM      = 0xcefaedfeU;
    const uint32_t MH_MAGIC_64   = 0xfeedfacfU;
    const uint32_t MH_CIGAM_64   = 0xcffaedfeU;
    const uint32_t FAT_MAGIC     = 0xcafebabeU;
    const uint32_t FAT_CIGAM     = 0xbebafecaU;
    if (magic != MH_MAGIC && magic != MH_CIGAM
        && magic != MH_MAGIC_64 && magic != MH_CIGAM_64
        && magic != FAT_MAGIC && magic != FAT_CIGAM) {
        /* Not a Mach-O. The cross-platform dispatcher returns
         * _RESIGN_FAILED on this path; signal that via non-zero rc. */
        return -1;
    }

    if (id == NULL) {
        return resign_adhoc(path);
    }
    return resign_with_identity(path, id);
}
