/** @file test/unit/test_attest_monocypher_smoke.c — Monocypher
 *        integration smoke regression.
 *
 *  WP-002 Phase 1 regression test for the Monocypher subproject
 *  integration. Proves the four conditions any future WP relies on:
 *
 *    1. The headers `<monocypher.h>` and `<monocypher-ed25519.h>`
 *       resolve through the libn00b consumer-side include path.
 *    2. The link wiring picks up the four primitives WP-002 phases
 *       2-3 will consume: `crypto_ed25519_key_pair`,
 *       `crypto_ed25519_sign`, `crypto_ed25519_check`, and
 *       `crypto_wipe`.
 *    3. The integration produces the same bytes the RFC 8032
 *       reference vectors produce — i.e. we're getting standards-
 *       compliant SHA-512 Ed25519, not silently dropping in some
 *       drift variant.
 *    4. The verifier returns nonzero on a tampered signature (a
 *       basic integrity smoke; not a substitute for the verifier
 *       WP's full negative-case coverage).
 *
 *  Vector chosen: RFC 8032 §7.1 test vector #1
 *  (https://datatracker.ietf.org/doc/html/rfc8032#section-7.1).
 *  Seed is the 32-byte all-zeros-ish standard vector; the message
 *  is empty; expected pubkey, secret-key expansion, and signature
 *  are the published values.
 *
 *  IMPORTANT NOTE on naming: Monocypher v4 has TWO Ed25519 APIs.
 *  The core-monocypher `crypto_eddsa_*` family (in monocypher.h)
 *  uses BLAKE2b internally — it is NOT RFC 8032 Ed25519 and does
 *  NOT produce the canonical published signatures. The RFC 8032
 *  SHA-512 variant lives in the optional `monocypher-ed25519`
 *  module as `crypto_ed25519_*`. WP-002 uses the SHA-512 variant
 *  exclusively (for interop with all standard verifiers).
 *
 *  Test-file conventions per D-030 — libc I/O for logging,
 *  <assert.h> for asserts, standard main signature.
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"

#include <monocypher.h>
#include <monocypher-ed25519.h>

// RFC 8032 §7.1 test vector #1.
static const uint8_t k_seed[32] = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
    0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
    0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
    0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
};

static const uint8_t k_expected_pubkey[32] = {
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
    0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
    0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
    0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
};

static const uint8_t k_expected_sig[64] = {
    0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72,
    0x90, 0x86, 0xe2, 0xcc, 0x80, 0x6e, 0x82, 0x8a,
    0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5, 0xd9, 0x74,
    0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55,
    0x5f, 0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac,
    0xc6, 0x1e, 0x39, 0x70, 0x1c, 0xf9, 0xb4, 0x6b,
    0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe, 0x24,
    0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b,
};

static void
test_keypair_derivation(uint8_t secret_key_out[64], uint8_t pubkey_out[32])
{
    uint8_t seed_mut[32];
    memcpy(seed_mut, k_seed, 32);  // crypto_ed25519_key_pair wipes the seed.

    crypto_ed25519_key_pair(secret_key_out, pubkey_out, seed_mut);

    assert(memcmp(pubkey_out, k_expected_pubkey, 32) == 0);

    // The seed slot should now be zeroed (Monocypher wipes the seed
    // argument as part of the key-pair derivation).
    uint8_t zero32[32] = {0};
    assert(memcmp(seed_mut, zero32, 32) == 0);

    printf("  [PASS] keypair_derivation\n");
}

static void
test_sign_matches_rfc8032(const uint8_t secret_key[64])
{
    uint8_t sig[64];
    // RFC 8032 vector #1 uses an empty message.
    crypto_ed25519_sign(sig, secret_key, nullptr, 0);

    if (memcmp(sig, k_expected_sig, 64) != 0) {
        fprintf(stderr, "FAIL sig mismatch\n  got: ");
        for (int i = 0; i < 64; i++) fprintf(stderr, "%02x", sig[i]);
        fprintf(stderr, "\n  exp: ");
        for (int i = 0; i < 64; i++) fprintf(stderr, "%02x", k_expected_sig[i]);
        fprintf(stderr, "\n");
        assert(0);
    }

    printf("  [PASS] sign_matches_rfc8032\n");
}

static void
test_verify_ok(const uint8_t pubkey[32])
{
    int rc = crypto_ed25519_check(k_expected_sig, pubkey, nullptr, 0);
    assert(rc == 0);  // 0 == valid in Monocypher's check convention.
    printf("  [PASS] verify_ok\n");
}

static void
test_verify_tampered(const uint8_t pubkey[32])
{
    uint8_t tampered[64];
    memcpy(tampered, k_expected_sig, 64);
    tampered[0] ^= 0x01;

    int rc = crypto_ed25519_check(tampered, pubkey, nullptr, 0);
    assert(rc != 0);  // nonzero == invalid.

    // Also try with a message that differs from what was signed.
    uint8_t one_byte = 0x00;
    rc = crypto_ed25519_check(k_expected_sig, pubkey, &one_byte, 1);
    assert(rc != 0);

    printf("  [PASS] verify_tampered\n");
}

static void
test_crypto_wipe_links(void)
{
    // The file-backend (Phase 2) will call crypto_wipe on the
    // expanded-sk buffer at release time. Smoke that the symbol
    // resolves through the link by calling it on a stack buffer.
    uint8_t buf[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    crypto_wipe(buf, sizeof(buf));
    for (int i = 0; i < 16; i++) {
        assert(buf[i] == 0);
    }
    printf("  [PASS] crypto_wipe_links\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    printf("== Monocypher Ed25519 (RFC 8032) integration smoke ==\n");

    uint8_t secret_key[64];
    uint8_t pubkey[32];

    test_keypair_derivation(secret_key, pubkey);
    test_sign_matches_rfc8032(secret_key);
    test_verify_ok(pubkey);
    test_verify_tampered(pubkey);
    test_crypto_wipe_links();

    // Clean up the secret-key buffer so the test binary doesn't leave
    // private bytes in its stack frame on the way out.
    crypto_wipe(secret_key, sizeof(secret_key));

    printf("All Monocypher integration smoke tests passed.\n");
    return 0;
}
